// kernel/include/fs/ext2.h
#ifndef _FS_EXT2_H
#define _FS_EXT2_H

#include <stdint.h>
#include <block/blockdev.h>
#include <fs/vfs.h>
#include <kernel/arch/x86_64/spinlock.h>

// ── Constants ──────────────────────────────────────────
#define EXT2_SB_OFFSET      1024
#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2
#define EXT2_S_IFREG        0x8000
#define EXT2_S_IFDIR        0x4000

// ── On-disk superblock (first 264 meaningful bytes of 1024-byte block) ──
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // Revision 1 fields (offset 84):
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
} ext2_superblock_t;

// ── Block Group Descriptor (32 bytes) ──────────────────
typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_bgdesc_t;

// ── Inode (first 128 bytes) ─────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
} ext2_inode_t;

// ── Directory Entry (variable-length, rec_len linked list) ──
typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];        // name_len bytes, no NUL terminator guaranteed
} ext2_dirent_t;

// ── Runtime filesystem context ─────────────────────────
typedef struct {
    block_device_t   *dev;
    uint32_t          block_size;
    uint32_t          sectors_per_block;
    uint32_t          inodes_per_group;
    uint32_t          blocks_per_group;
    uint32_t          num_block_groups;
    uint32_t          inode_size;
    ext2_bgdesc_t    *bgdesc_table;
    spinlock_T        lock;        // ★ spinlock_T (capital T), see spinlock.h:10
} ext2_fs_t;

// ── API ────────────────────────────────────────────────
int ext2_init(block_device_t *dev, ext2_fs_t **out_fs);
extern struct vfs_ops ext2_vfs_ops;

#endif
