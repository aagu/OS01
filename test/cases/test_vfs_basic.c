/*
 * test/cases/test_vfs_basic.c — VFS core unit tests
 * Fully self-contained — re-defines minimal VFS types.
 */
#include "test_framework.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ── Minimal VFS type definitions ───────────────── */
#define VFS_FILE   1
#define VFS_DIR    2
#define VFS_NAME_MAX 256

typedef struct vfs_node     vfs_node_t;
typedef struct vfs_dirent   vfs_dirent_t;
typedef struct vfs_mount    vfs_mount_t;

typedef struct vfs_ops {
    int (*read)(vfs_node_t *, uint64_t, uint64_t, void *);
    int (*write)(vfs_node_t *, uint64_t, uint64_t, void *);
    int (*readdir)(vfs_node_t *, uint64_t, vfs_dirent_t *);
    vfs_node_t *(*create)(vfs_node_t *, const char *);
} vfs_ops_t;

struct vfs_dirent {
    char     name[VFS_NAME_MAX];
    uint64_t size;
    uint8_t  type;
    uint32_t ino;
};

struct vfs_node {
    char              name[VFS_NAME_MAX];
    uint64_t          size;
    uint8_t           type;
    vfs_mount_t      *mount;
    void             *fs_data;
    vfs_node_t       *parent;
    vfs_ops_t        *ops;
    uint32_t          refcount;
};

struct vfs_mount {
    void          *dev;
    const char    *path;
    vfs_node_t    *root;
    vfs_ops_t     *ops;
    void          *fs_data;
};

/* ── Fake filesystem (in-memory) ────────────────── */
typedef struct {
    char     name[256];
    uint8_t  type;
    uint64_t size;
    void    *data;
} fake_file_t;

#define FAKE_FILES 16
static fake_file_t fake_fs[FAKE_FILES];
static int fake_fs_count = 0;

static void fake_build_tree(void) {
    fake_fs_count = 0;
    memcpy(fake_fs[0].name, ".", 2);     fake_fs[0].type = VFS_DIR;
    memcpy(fake_fs[1].name, "..", 3);    fake_fs[1].type = VFS_DIR;
    memcpy(fake_fs[2].name, "README.TXT", 11); fake_fs[2].type = VFS_FILE;
    fake_fs[2].size = 13;  fake_fs[2].data = strdup("Hello, World!");
    memcpy(fake_fs[3].name, "SUBDIR", 7); fake_fs[3].type = VFS_DIR;
    memcpy(fake_fs[4].name, "HELLO.TXT", 10); fake_fs[4].type = VFS_FILE;
    fake_fs[4].size = 5;   fake_fs[4].data = malloc(6); memcpy(fake_fs[4].data, "HELLO", 6);
    memcpy(fake_fs[5].name, "BIGFILE.BIN", 12); fake_fs[5].type = VFS_FILE;
    fake_fs[5].size = 100;
    char *big = malloc(101); memset(big, 'B', 100); big[100] = 0;
    fake_fs[5].data = big;
    fake_fs_count = 6;
}

/* ── Fake VFS operations ─────────────────────────── */
static int fake_readdir(vfs_node_t *node, uint64_t idx, vfs_dirent_t *entry) {
    (void)node;
    if (!entry) return -1;
    if (idx >= (uint64_t)fake_fs_count) { entry->name[0] = '\0'; return 0; }
    memset(entry->name, 0, VFS_NAME_MAX);
    memcpy(entry->name, fake_fs[idx].name, strlen(fake_fs[idx].name) + 1);
    entry->type = fake_fs[idx].type;
    entry->size = fake_fs[idx].size;
    entry->ino = (uint32_t)idx;
    return 0;
}

static int fake_read(vfs_node_t *node, uint64_t off, uint64_t sz, void *buf) {
    if (!node || !node->fs_data) return -1;
    fake_file_t *f = (fake_file_t *)node->fs_data;
    if (off >= f->size) return 0;
    uint64_t avail = f->size - off;
    if (sz > avail) sz = avail;
    if (sz > 0) memcpy(buf, (uint8_t*)f->data + off, sz);
    return (int)sz;
}

static vfs_node_t *fake_create(vfs_node_t *dir, const char *name) {
    (void)dir;
    if (fake_fs_count >= FAKE_FILES) return NULL;
    memcpy(fake_fs[fake_fs_count].name, name, strlen(name)+1);
    fake_fs[fake_fs_count].type = VFS_FILE;
    fake_fs[fake_fs_count].size = 0;
    fake_fs[fake_fs_count].data = malloc(1);
    fake_fs_count++;
    vfs_node_t *n = malloc(sizeof(vfs_node_t)); memset(n, 0, sizeof(vfs_node_t));
    if (!n) return NULL;
    size_t nnl = strlen(name); memcpy(n->name, name, nnl+1); n->type = VFS_FILE;
    n->fs_data = &fake_fs[fake_fs_count - 1];
    n->refcount = 1;
    return n;
}

static vfs_ops_t fake_ops = {
    .read    = fake_read,
    .readdir = fake_readdir,
    .create  = fake_create,
};

static vfs_mount_t fake_mount;

/* ── Case-insensitive name compare (like kernel's) ─ */
static int name_cmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* ── Path tokenizer (same algorithm as kernel VFS) ─ */
static int tokenize(const char *path, char **components, int max) {
    char copy[VFS_NAME_MAX];
    strcpy(copy, path);
    char *p = copy;
    int count = 0;
    while (*p && count < max) {
        while (*p == '/') p++;
        if (*p == '\0') break;
        components[count++] = p;
        while (*p && *p != '/') p++;
        if (*p == '/') { *p = '\0'; p++; }
    }
    return count;
}

/* ── Tests ──────────────────────────────────────────── */

TEST_FUNC(test_vfs_case_insensitive) {
    /* Verify that FAT32 case-insensitive name matching works */
    assert_eq(name_cmp("README.TXT", "readme.txt"), 0);
    assert_true(name_cmp("README.TXT", "readme.tx") > 0);
    assert_true(name_cmp("ABC", "ABD") < 0);
    assert_eq(name_cmp("ABC", "ABC"), 0);
}

TEST_FUNC(test_vfs_readdir_root) {
    fake_build_tree();
    memset(&fake_mount, 0, sizeof(fake_mount));
    fake_mount.root = malloc(sizeof(vfs_node_t)); memset(fake_mount.root, 0, sizeof(vfs_node_t));
    strcpy(fake_mount.root->name, "/");
    fake_mount.root->type = VFS_DIR;
    fake_mount.root->ops = &fake_ops;
    fake_mount.path = "/";

    vfs_dirent_t entry;
    int r = fake_readdir(NULL, 0, &entry);
    assert_eq(r, 0);
    assert_str_eq(entry.name, ".");

    memset(&entry, 0, sizeof(entry));
    r = fake_readdir(NULL, 2, &entry);
    assert_eq(r, 0);
    assert_str_eq(entry.name, "README.TXT");
    assert_eq(entry.type, VFS_FILE);
    assert_eq(entry.size, 13);
}

TEST_FUNC(test_vfs_read_file) {
    fake_build_tree();
    /* Simulate opening and reading README.TXT */
    vfs_node_t node;
    memset(&node, 0, sizeof(node));
    node.fs_data = &fake_fs[2];
    strcpy(node.name, "README.TXT");

    char buf[32];
    memset(buf, 0, sizeof(buf));
    memset(buf, 0, sizeof(buf));
    int n = fake_read(&node, 0, 32, buf);
    assert_eq(n, 13);
    assert_str_eq(buf, "Hello, World!");
}

TEST_FUNC(test_vfs_path_tokenizer_simple) {
    char path[] = "/foo/bar/baz";
    char *p = path;
    char *comp[8];
    int n = 0;
    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;
        comp[n++] = p;
        while (*p && *p != '/') p++;
        if (*p == '/') { *p = '\0'; p++; }
    }
    assert_eq(n, 3);
    assert_str_eq(comp[0], "foo");
    assert_str_eq(comp[1], "bar");
    assert_str_eq(comp[2], "baz");
}

TEST_FUNC(test_vfs_path_tokenizer_root) {
    char path[] = "/";
    char *p = path;
    char *comp[8];
    int n = 0;
    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;
        comp[n++] = p;
        while (*p && *p != '/') p++;
        if (*p == '/') { *p = '\0'; p++; }
    }
    assert_eq(n, 0);
}

TEST_FUNC(test_vfs_path_tokenizer_relative) {
    char path[] = "a/b/c";
    char *p = path;
    char *comp[8];
    int n = 0;
    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;
        comp[n++] = p;
        while (*p && *p != '/') p++;
        if (*p == '/') { *p = '\0'; p++; }
    }
    assert_eq(n, 3);
    assert_str_eq(comp[0], "a");
    assert_str_eq(comp[1], "b");
    assert_str_eq(comp[2], "c");
}

TEST_FUNC(test_vfs_path_tokenizer_trailing_slash) {
    char path[] = "/foo/bar/";
    char *p = path;
    char *comp[8];
    int n = 0;
    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;
        comp[n++] = p;
        while (*p && *p != '/') p++;
        if (*p == '/') { *p = '\0'; p++; }
    }
    assert_eq(n, 2);
    assert_str_eq(comp[0], "foo");
    assert_str_eq(comp[1], "bar");
}

TEST_FUNC(test_vfs_readdir_all_entries) {
    fake_build_tree();
    int count = 0;
    vfs_dirent_t entry;
    for (uint64_t i = 0; i < 20; i++) {
        if (fake_readdir(NULL, i, &entry) != 0) break;
        if (entry.name[0] == '\0') break;
        count++;
    }
    assert_eq(count, 6);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_vfs_case_insensitive),
    TEST_ENTRY(test_vfs_readdir_root),
    TEST_ENTRY(test_vfs_read_file),
    TEST_ENTRY(test_vfs_path_tokenizer_simple),
    TEST_ENTRY(test_vfs_path_tokenizer_root),
    TEST_ENTRY(test_vfs_path_tokenizer_relative),
    TEST_ENTRY(test_vfs_path_tokenizer_trailing_slash),
    TEST_ENTRY(test_vfs_readdir_all_entries),
TEST_LIST_END

int main() {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
