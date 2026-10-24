# ARCH-5 Phase 1: Filesystem Type Registry & Mount-by-Type Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce the `vfs_fs_type_t` filesystem registration table and `vfs_mount_fs()` mount-by-type API, register all five built-in filesystems (`devfs`, `ext2`, `vfat`, `tmpfs`, `procfs`), and migrate boot/init mounting away from raw `vfs_ops_t` globals while keeping `vfs_ops_t` as the Phase 1 transition layer.

**Architecture:** `kernel/fs/vfs.c` gains an rwlock-protected `vfs_fs_type_t` registry with per-type active/in-flight mount usage counting (`vfs_register_filesystem`, `vfs_unregister_filesystem`, `vfs_find_filesystem`, `vfs_mount_fs`). Each of the five filesystems (`devfs`, `tmpfs`, `procfs`, `ext2`, `fat`) encapsulates its mount initialization behind a static `vfs_fs_type_t` and exposes an `xxx_register_fs()` entry point invoked by `vfs_init()`. `kernel/fs/boot.c` mounts `"ext2"` and `"vfat"` by type name via `vfs_mount_fs()`.

**Tech Stack:** Freestanding C11 (`clang`, `lld`), OS01 kernel VFS (`kernel/fs/`), host unit-test runner (`hosttests/`), QEMU integration harness (`qemutests/`).

**Spec:** [`docs/superpowers/specs/2026-10-24-arch5-vfs-abstraction-design.md`](../specs/2026-10-24-arch5-vfs-abstraction-design.md) (§3, §4.1, §5.1, §5.5, §6, §7)

## Global Constraints

- **Symmetric header/source layout (`AGENTS.md`)**: Public headers for `kernel/fs/*.c` live in `kernel/include/fs/*.h` and are included as `<fs/foo.h>`.
- **Mandatory clean rebuild after struct changes (`AGENTS.md`)**: Because `struct vfs_mount` in `kernel/include/fs/vfs.h` changes layout, `make clean` MUST be executed before building kernel images.
- **Phase 1 transition boundary (Spec §5.5)**: Phase 1 retains `vfs_ops_t` and `vfs_mount()` as transitional compatibility; `vfs_fs_type_t.mount` populates `mp->ops` and `mp->root->ops`. Do NOT introduce `i_ops`/`f_ops`, `active_nodes`, node-attached mount trees, or `dcache` in Phase 1.
- **FS type lifetime & reservation contract (Spec §5.5)**: Built-in `vfs_fs_type_t` descriptors use static storage; `vfs_find_filesystem()` returns a non-owning `const vfs_fs_type_t *`. `vfs_mount_fs()` reserves a usage count on the matched `vfs_fs_type_t` under the registry lock before unlocking to call `fs_type->mount()`; on failure it cleans up `mp`/`mp->root` and drops the reservation, and on success the reservation becomes the mounted instance's reference. `vfs_unregister_filesystem()` returns `-EBUSY` whenever the usage count is non-zero (covering both in-flight `mount()` calls and published mounts).
- **Logging policy (`AGENTS.md`)**: Use `log_err`/`log_warn`/`log_info`/`log_debug` or `debug_vfs`/`debug_fs`; never use bare `serial_printk` or `color_printk`. Preserve exact boot log strings in `kernel/fs/boot.c`.

## Review Focus

1. **In-flight mount unregistration race**: Calling `vfs_unregister_filesystem("type")` while `fs_type->mount()` is executing must return `-EBUSY` because `vfs_mount_fs` reserves the type usage count before releasing the registry lock (tested in Task 1 `test_fs_unregister_busy_and_in_progress`).
2. **Failed `mount()` callback resource & reservation rollback**: When `fs_type->mount(mp, dev, data)` returns a negative `-errno`, `vfs_mount_fs()` must free `mp->root->name`, `mp->root`, `mp->path`, and `mp`, drop the type usage reservation so `vfs_unregister_filesystem()` succeeds (`0`), and leave `mount_list` unchanged (tested in Task 1 `test_fs_mount_validation_and_rollback`).
3. **Invalid or relative target path in `vfs_mount_fs()`**: Passing `NULL`, `""`, or a relative path (`"mnt/usb"`) to `vfs_mount_fs()` must return `-EINVAL` without invoking `fs_type->mount()` (tested in Task 1 `test_fs_mount_validation_and_rollback`).
4. **Idempotent `vfs_init()` re-registration**: Calling `vfs_init()` multiple times in host tests must reset `fs_type_list` and `next` pointers cleanly without creating cycles in static `vfs_fs_type_t` structs (tested in Task 2 `test_builtin_filesystems_registered_on_init`).
5. **Non-fatal `/boot` mount failure vs fatal `/` mount failure**: In `kernel/fs/boot.c`, if `"ext2"` mounts `"/"` (`0`) but `"vfat"` fails on `"/boot"` (`< 0`), `fs_boot_mounts()` must log a warning and continue without panicking; if `"/"` fails, it must `kpanic("root filesystem unavailable\n")` (tested in Task 3 `test_root_requirement`).

---

## File Structure Map

| File | Action | Responsibility in Phase 1 |
|------|--------|---------------------------|
| `kernel/include/fs/vfs.h` | Modify | Define `VFS_FS_*` flags, `vfs_fs_type_t`, add `fs_type`/`mnt_flags` to `vfs_mount_t`, declare `vfs_register_filesystem`, `vfs_unregister_filesystem`, `vfs_find_filesystem`, `vfs_mount_fs`. |
| `kernel/fs/vfs.c` | Modify | Implement `vfs_is_valid_kptr()` (host-test friendly), `fs_type_list` + `fs_type_lock` + usage reservation table, registry APIs, `vfs_mount_fs()`, and call the 5 `*_register_fs()` hooks from `vfs_init()`. |
| `kernel/include/fs/devfs.h` | Modify | Declare `int devfs_register_fs(void);`. |
| `kernel/fs/devfs.c` | Modify | Implement `devfs_fs_mount()`, `devfs_register_fs()`, and switch `devfs_init()` to `vfs_mount_fs("devfs", "/dev", NULL, NULL)`. |
| `kernel/include/fs/tmpfs.h` | Modify | Declare `int tmpfs_register_fs(void);`. |
| `kernel/fs/tmpfs.c` | Modify | Implement `tmpfs_fs_mount()`, `tmpfs_register_fs()`, and switch `tmpfs_init()` to `vfs_mount_fs("tmpfs", "/tmp", NULL, NULL)`. |
| `kernel/include/fs/procfs.h` | Modify | Declare `int procfs_register_fs(void);`. |
| `kernel/fs/procfs.c` | Modify | Implement `procfs_fs_mount()`, `procfs_register_fs()`, and switch `procfs_init()` to `vfs_mount_fs("procfs", "/proc", NULL, NULL)`. |
| `kernel/include/fs/ext2.h` | Modify | Declare `int ext2_register_fs(void);`. |
| `kernel/fs/ext2.c` | Modify | Implement `ext2_fs_mount()` and `ext2_register_fs()`. |
| `kernel/include/fs/fat.h` | Modify | Declare `int fat_register_fs(void);`. |
| `kernel/fs/fat.c` | Modify | Implement `fat_fs_mount()` and `fat_register_fs()`. |
| `kernel/fs/boot.c` | Modify | Replace manual `ext2_init`/`fat32_init` + `vfs_mount` in `mount_partitioned_disk()` with `vfs_mount_fs("ext2", ...)` and `vfs_mount_fs("vfat", ...)`. |
| `hosttests/mock/vfs_test_runtime.h` | Create | Host preinclude header for compiling production `kernel/fs/vfs.c` in host tests. |
| `hosttests/cases/test_vfs_fs_registry.c` | Create | Unit test suite for Phase 1 registry and `vfs_mount_fs` contracts. |
| `hosttests/cases/test_devfs_input_dispatch.c` | Modify | Add `vfs_register_filesystem` and `vfs_mount_fs` shims so `devfs_init()` continues to populate `devfs_mount_ops`. |
| `hosttests/cases/test_device_boot.c` | Modify | Add `vfs_mount_fs` mock to match `kernel/fs/boot.c` and test `/boot` non-fatal failure. |
| `hosttests/Makefile` | Modify | Build `vfs_production.o` and `test_vfs_fs_registry.elf`, add to `TEST_BINS`. |

---

### Task 1: Core VFS Filesystem Registry & `vfs_mount_fs`

**Files:**
- Modify: `kernel/include/fs/vfs.h:110-150`
- Modify: `kernel/fs/vfs.c:11-92,196-205,390-405,777-812,970-980,1110-1240`
- Create: `hosttests/mock/vfs_test_runtime.h`
- Create: `hosttests/cases/test_vfs_fs_registry.c`
- Modify: `hosttests/Makefile:70-75,540-550`

**Interfaces:**
- Consumes: `rwlock_t`, `rwlock_init`, `rwlock_read_lock`, `rwlock_read_unlock`, `rwlock_write_lock`, `rwlock_write_unlock` from `<sync/rwlock.h>`.
- Produces (in `kernel/include/fs/vfs.h`):
  ```c
  #define VFS_FS_REQUIRES_DEV      (1u << 0)
  #define VFS_FS_CASE_INSENSITIVE  (1u << 1)
  #define VFS_FS_NO_DCACHE         (1u << 2)

  typedef struct vfs_fs_type {
      const char         *name;
      uint32_t            fs_flags;
      int               (*mount)(struct vfs_mount *mp, block_device_t *dev, void *data);
      void              (*unmount)(struct vfs_mount *mp);
      struct vfs_fs_type *next;
  } vfs_fs_type_t;

  int                 vfs_register_filesystem(vfs_fs_type_t *fs_type);
  int                 vfs_unregister_filesystem(const char *name);
  const vfs_fs_type_t *vfs_find_filesystem(const char *name);
  int                 vfs_mount_fs(const char *type_name, const char *path,
                                   block_device_t *dev, void *data);
  ```
  And on `struct vfs_mount`:
  ```c
  const vfs_fs_type_t *fs_type;
  uint32_t             mnt_flags;
  ```

- [ ] **Step 1: Create `hosttests/mock/vfs_test_runtime.h` and `hosttests/cases/test_vfs_fs_registry.c`, and wire `hosttests/Makefile`**

Create `hosttests/mock/vfs_test_runtime.h`:
```c
#ifndef OS01_VFS_TEST_RUNTIME_H
#define OS01_VFS_TEST_RUNTIME_H

#include "test_platform.h"
#include <list.h>
#include <uapi/time.h>

#define KERNEL_TASK_H
#define _KERNEL_PERCPU_H
#define _ARCH_IRQ_H
#define _ARCH_CPU_H
#define _ARCH_SPINLOCK_H
#define _KERNEL_SLAB_H
#define _KERNEL_MEMORY_H

struct files_struct;

typedef struct task_struct {
    list_t list;
    volatile int64_t state;
    uint64_t addr_limit;
    int64_t pid;
    pid_t pgrp;
    pid_t session;
    int64_t signal;
    int64_t blocked;
    struct files_struct *files;
    list_t io_wait_node;
    int ctty_type;
    void *ctty;
} task_t;

#include <fs/file.h>

extern task_t *vfs_test_current;
#define current vfs_test_current

void *kmalloc(size_t size);
size_t kfree(void *ptr);

#endif /* OS01_VFS_TEST_RUNTIME_H */
```

Create `hosttests/cases/test_vfs_fs_registry.c`:
```c
#include "test_framework.h"
#include <fs/vfs.h>
#include <block/blockdev.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

task_t *vfs_test_current = NULL;
int g_log_level = 0;
void _log_write(int level, const char *fmt, ...) { (void)level; (void)fmt; }

static int s_mock_mount_calls = 0;
static int s_mock_mount_ret = 0;
static int s_unreg_inside_mount_rc = 0;
static bool s_try_unreg_inside_mount = false;
static vfs_ops_t s_dummy_ops = {0};

static int mock_fs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    (void)dev;
    s_mock_mount_calls++;
    if (s_try_unreg_inside_mount && mp && mp->fs_type) {
        s_unreg_inside_mount_rc = vfs_unregister_filesystem(mp->fs_type->name);
    }
    if (s_mock_mount_ret < 0) {
        return s_mock_mount_ret;
    }
    mp->ops = &s_dummy_ops;
    mp->fs_data = data;
    if (mp->root) {
        mp->root->ops = &s_dummy_ops;
    }
    return 0;
}

TEST_FUNC(test_fs_register_and_find) {
    vfs_init();

    vfs_fs_type_t custom = {
        .name = "customfs",
        .fs_flags = VFS_FS_NO_DCACHE,
        .mount = mock_fs_mount,
    };

    assert_eq(0, vfs_register_filesystem(&custom));
    const vfs_fs_type_t *found = vfs_find_filesystem("customfs");
    assert_true(found == &custom);
    assert_eq(VFS_FS_NO_DCACHE, found->fs_flags);

    assert_null(vfs_find_filesystem("nonexistent"));
    assert_null(vfs_find_filesystem(""));
    assert_null(vfs_find_filesystem(NULL));

    assert_eq(0, vfs_unregister_filesystem("customfs"));
    assert_null(vfs_find_filesystem("customfs"));
}

TEST_FUNC(test_fs_register_invalid_and_duplicate) {
    vfs_init();

    vfs_fs_type_t no_name = { .name = NULL, .mount = mock_fs_mount };
    vfs_fs_type_t empty_name = { .name = "", .mount = mock_fs_mount };
    vfs_fs_type_t no_mount = { .name = "nomount", .mount = NULL };
    vfs_fs_type_t valid1 = { .name = "dupfs", .mount = mock_fs_mount };
    vfs_fs_type_t valid2 = { .name = "dupfs", .mount = mock_fs_mount };

    assert_eq(-EINVAL, vfs_register_filesystem(NULL));
    assert_eq(-EINVAL, vfs_register_filesystem(&no_name));
    assert_eq(-EINVAL, vfs_register_filesystem(&empty_name));
    assert_eq(-EINVAL, vfs_register_filesystem(&no_mount));

    assert_eq(0, vfs_register_filesystem(&valid1));
    assert_eq(-EEXIST, vfs_register_filesystem(&valid2));
    assert_eq(-EINVAL, vfs_unregister_filesystem(NULL));
    assert_eq(-EINVAL, vfs_unregister_filesystem(""));
    assert_eq(-ENOENT, vfs_unregister_filesystem("missing_fs"));
}

TEST_FUNC(test_fs_mount_validation_and_rollback) {
    vfs_init();
    s_mock_mount_calls = 0;
    s_mock_mount_ret = 0;
    s_try_unreg_inside_mount = false;

    vfs_fs_type_t dev_fs = {
        .name = "mockblk",
        .fs_flags = VFS_FS_REQUIRES_DEV,
        .mount = mock_fs_mount,
    };
    assert_eq(0, vfs_register_filesystem(&dev_fs));

    /* Invalid args & relative path rejected before mount() callback */
    assert_eq(-EINVAL, vfs_mount_fs(NULL, "/mnt", NULL, NULL));
    assert_eq(-EINVAL, vfs_mount_fs("", "/mnt", NULL, NULL));
    assert_eq(-EINVAL, vfs_mount_fs("mockblk", NULL, NULL, NULL));
    assert_eq(-EINVAL, vfs_mount_fs("mockblk", "", NULL, NULL));
    assert_eq(-EINVAL, vfs_mount_fs("mockblk", "relative/path", NULL, NULL));
    assert_eq(-ENODEV, vfs_mount_fs("unknown_fs", "/mnt", NULL, NULL));

    /* REQUIRES_DEV with NULL block_device_t -> -EINVAL */
    assert_eq(-EINVAL, vfs_mount_fs("mockblk", "/mnt", NULL, NULL));
    assert_eq(0, s_mock_mount_calls);

    /* Failing mount() propagates -errno, does not publish mount, releases reservation */
    block_device_t fake_bdev;
    memset(&fake_bdev, 0, sizeof(fake_bdev));
    s_mock_mount_ret = -EIO;
    assert_eq(-EIO, vfs_mount_fs("mockblk", "/fail", &fake_bdev, NULL));
    assert_eq(1, s_mock_mount_calls);
    assert_null(vfs_lookup("/fail"));
    assert_eq(0, vfs_unregister_filesystem("mockblk"));
}

TEST_FUNC(test_fs_unregister_busy_and_in_progress) {
    vfs_init();
    s_mock_mount_calls = 0;
    s_mock_mount_ret = 0;
    s_try_unreg_inside_mount = true;
    s_unreg_inside_mount_rc = 0;

    vfs_fs_type_t mem_fs = {
        .name = "mockmem",
        .fs_flags = VFS_FS_CASE_INSENSITIVE,
        .mount = mock_fs_mount,
    };
    assert_eq(0, vfs_register_filesystem(&mem_fs));

    int tag = 42;
    assert_eq(0, vfs_mount_fs("mockmem", "/", NULL, &tag));
    /* Unregister attempted inside mount() must have seen -EBUSY */
    assert_eq(-EBUSY, s_unreg_inside_mount_rc);
    /* Unregister while mounted must also return -EBUSY */
    assert_eq(-EBUSY, vfs_unregister_filesystem("mockmem"));

    vfs_node_t *root = vfs_lookup("/");
    assert_not_null(root);
    assert_not_null(root->mount);
    assert_true(root->mount->fs_type == &mem_fs);
    assert_eq(VFS_FS_CASE_INSENSITIVE, root->mount->mnt_flags);
    assert_true(root->mount->fs_data == &tag);
    vfs_node_put(root);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_fs_register_and_find),
    TEST_ENTRY(test_fs_register_invalid_and_duplicate),
    TEST_ENTRY(test_fs_mount_validation_and_rollback),
    TEST_ENTRY(test_fs_unregister_busy_and_in_progress),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
```

In `hosttests/Makefile`:
1. Add `$(TEST_BLD)/test_vfs_fs_registry.elf \` to `TEST_BINS` right after `$(TEST_BLD)/test_vfs_basic.elf \`.
2. Add the build rules right after the `$(TEST_BLD)/test_vfs_basic.elf:` rule:
```makefile
VFS_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
    -include $(TEST_MOCK)/vfs_test_runtime.h \
    -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/vfs_production.o: $(TESTS_DIR)/kernel/fs/vfs.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_vfs_fs_registry.o: $(TEST_CASES)/test_vfs_fs_registry.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_vfs_fs_registry.elf: $(TEST_BLD)/test_vfs_fs_registry.o \
		$(TEST_BLD)/vfs_production.o \
		$(TEST_BLD)/rwlock.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^
```

- [ ] **Step 2: Run the new host test to verify it fails (RED)**

Run: `make test-host`
Expected: Compilation of `test_vfs_fs_registry.c` fails with unknown type `vfs_fs_type_t`, undeclared `VFS_FS_NO_DCACHE` / `VFS_FS_REQUIRES_DEV` / `VFS_FS_CASE_INSENSITIVE`, and implicit declaration of `vfs_register_filesystem`, `vfs_unregister_filesystem`, `vfs_find_filesystem`, `vfs_mount_fs`.

- [ ] **Step 3: Implement `vfs_fs_type_t` and registry / `vfs_mount_fs` in `kernel/include/fs/vfs.h` and `kernel/fs/vfs.c`**

1. In `kernel/include/fs/vfs.h`:
   - Add `struct vfs_fs_type;` to forward declarations.
   - Define `VFS_FS_REQUIRES_DEV`, `VFS_FS_CASE_INSENSITIVE`, `VFS_FS_NO_DCACHE` and `typedef struct vfs_fs_type { ... } vfs_fs_type_t;` per Spec §4.1.
   - Add `const vfs_fs_type_t *fs_type;` and `uint32_t mnt_flags;` to `struct vfs_mount`.
   - Declare `vfs_register_filesystem`, `vfs_unregister_filesystem`, `vfs_find_filesystem`, and `vfs_mount_fs` per Spec §4.1.
2. In `kernel/fs/vfs.c`:
   - Add a helper right above `find_mount`:
     ```c
     static inline bool vfs_is_valid_kptr(const void *p)
     {
     #ifdef OS01_HOST_TEST
         return p != NULL;
     #else
         return (uintptr_t)p >= 0xffff800000000000ULL;
     #endif
     }
     ```
     and replace the hardcoded `< 0xffff800000000000UL` / `>= 0xffff800000000000ULL` address checks in `vfs.c` (`find_mount`, `__vfs_lookup_raw`, `vfs_read`, `vfs_write`, `vfs_readdir`, `vfs_getdents`, `vfs_unlink`, `vfs_mkdir`, `vfs_rmdir`, `vfs_rename`, `vfs_truncate`) with `vfs_is_valid_kptr(...)`. Also in `__vfs_lookup_raw` and `vfs_getdents`, treat a directory as case-insensitive when `(cur->mount && (cur->mount->mnt_flags & VFS_FS_CASE_INSENSITIVE))` in addition to `(cur->ops && vfs_is_valid_kptr(cur->ops) && (cur->ops->flags & VFS_OPS_CASE_INSENSITIVE))`.
   - Add the registry state in `kernel/fs/vfs.c`:
     ```c
     #define VFS_MAX_FS_TYPES 16
     static vfs_fs_type_t *fs_type_list = NULL;
     static rwlock_t fs_type_lock;
     struct vfs_fs_usage_slot {
         const vfs_fs_type_t *type;
         uint32_t             active_count;
     };
     static struct vfs_fs_usage_slot fs_type_usage[VFS_MAX_FS_TYPES];
     ```
   - In `vfs_init()`: initialize `fs_type_list = NULL;`, `memset(fs_type_usage, 0, sizeof(fs_type_usage));`, and `rwlock_init(&fs_type_lock);`.
   - Implement `vfs_register_filesystem`, `vfs_unregister_filesystem`, `vfs_find_filesystem`, and `vfs_mount_fs` following Spec §4.1 and §5.5:
     - `vfs_register_filesystem(vfs_fs_type_t *fs_type)`: validate `fs_type && fs_type->name && fs_type->name[0] && fs_type->mount` (`-EINVAL`); under `rwlock_write_lock(&fs_type_lock)`, reject duplicate `name` (`-EEXIST`), allocate a zeroed slot in `fs_type_usage` (`-ENOMEM` if full), link `fs_type->next = fs_type_list; fs_type_list = fs_type;`, unlock and return `0`.
     - `vfs_find_filesystem(const char *name)`: if `!name || !name[0]` return `NULL`; under `rwlock_read_lock(&fs_type_lock)`, scan `fs_type_list` by `strcmp`, unlock and return match or `NULL`.
     - `vfs_unregister_filesystem(const char *name)`: if `!name || !name[0]` return `-EINVAL`; under `rwlock_write_lock(&fs_type_lock)`, locate `name` (`-ENOENT` if absent), check its `fs_type_usage` slot (`-EBUSY` if `active_count > 0`), unlink from `fs_type_list`, clear `next = NULL` and clear the `fs_type_usage` slot, unlock and return `0`.
     - `vfs_mount_fs(const char *type_name, const char *path, block_device_t *dev, void *data)`:
       - Validate `type_name && type_name[0] && path && path[0] == '/'` (`-EINVAL`).
       - If `!vfs_initialized`, call `vfs_init()`.
       - Under `rwlock_write_lock(&fs_type_lock)`: find `type_name` (`-ENODEV` if not found); if `(fs_type->fs_flags & VFS_FS_REQUIRES_DEV) && !dev`, unlock and return `-EINVAL`; increment `active_count` in `fs_type_usage`; unlock.
       - Allocate `mp = calloc(1, sizeof(vfs_mount_t))`, `path_copy = strdup(path)`, `mp->root = calloc(1, sizeof(vfs_node_t))`, `mp->root->name = strdup("/")`; on any allocation failure, free partial allocations, decrement `active_count` under `fs_type_lock`, and return `-ENOMEM`.
       - Set `mp->dev = dev`, `mp->fs_type = fs_type`, `mp->mnt_flags = fs_type->fs_flags`, `mp->path = path_copy`, `mp->root->mount = mp`, `mp->root->type = VFS_DIR`, `mp->root->refcount = 1`.
       - Call `int rc = fs_type->mount(mp, dev, data);`. If `rc < 0` or `!mp->root`: free `mp->root->name` and `mp->root` (if `mp->root` still non-NULL), free `path_copy`, free `mp`, decrement `active_count` under `fs_type_lock`, and return `rc < 0 ? rc : -EIO`.
       - Synchronize `mp->root->mount = mp;` and `if (mp->ops && !mp->root->ops) mp->root->ops = mp->ops; else if (!mp->ops && mp->root->ops) mp->ops = mp->root->ops;`.
       - Publish `mp` onto `mount_list` under `rwlock_write_lock(&mount_lock)` and return `0`.

- [ ] **Step 4: Run `make test-host` to verify Task 1 passes (GREEN)**

Run: `make test-host`
Expected: `test_vfs_fs_registry.elf` compiles, links, and passes all 4 tests; all host test binaries pass.

- [ ] **Step 5: Commit Task 1**

```bash
git add kernel/include/fs/vfs.h kernel/fs/vfs.c hosttests/mock/vfs_test_runtime.h hosttests/cases/test_vfs_fs_registry.c hosttests/Makefile
git commit -m "feat(vfs): add vfs_fs_type_t registry and vfs_mount_fs (ARCH-5 Phase 1)"
```

---

### Task 2: Register Built-in Filesystems (`devfs`, `tmpfs`, `procfs`, `ext2`, `vfat`) & Wire `vfs_init()`

**Files:**
- Modify: `kernel/include/fs/devfs.h:70-76`
- Modify: `kernel/fs/devfs.c:640-666`
- Modify: `kernel/include/fs/tmpfs.h:1-6`
- Modify: `kernel/fs/tmpfs.c:425-455`
- Modify: `kernel/include/fs/procfs.h:30-35`
- Modify: `kernel/fs/procfs.c:520-534`
- Modify: `kernel/include/fs/ext2.h:125-130`
- Modify: `kernel/fs/ext2.c:1615-1625`
- Modify: `kernel/include/fs/fat.h:95-102`
- Modify: `kernel/fs/fat.c:1340-1347`
- Modify: `kernel/fs/vfs.c:1-15,32-45`
- Modify: `hosttests/cases/test_devfs_input_dispatch.c:90-105`
- Modify: `hosttests/cases/test_vfs_fs_registry.c`
- Modify: `hosttests/Makefile`

**Interfaces:**
- Consumes: `vfs_register_filesystem`, `vfs_mount_fs`, `vfs_fs_type_t`, `VFS_FS_*` from Task 1.
- Produces:
  - `int devfs_register_fs(void);` in `<fs/devfs.h>` (`name = "devfs"`, `fs_flags = VFS_FS_NO_DCACHE`)
  - `int ext2_register_fs(void);` in `<fs/ext2.h>` (`name = "ext2"`, `fs_flags = VFS_FS_REQUIRES_DEV`)
  - `int fat_register_fs(void);` in `<fs/fat.h>` (`name = "vfat"`, `fs_flags = VFS_FS_REQUIRES_DEV | VFS_FS_CASE_INSENSITIVE`)
  - `int tmpfs_register_fs(void);` in `<fs/tmpfs.h>` (`name = "tmpfs"`, `fs_flags = 0`)
  - `int procfs_register_fs(void);` in `<fs/procfs.h>` (`name = "procfs"`, `fs_flags = VFS_FS_NO_DCACHE`)

- [ ] **Step 1: Add `test_builtin_filesystems_registered_on_init` to `hosttests/cases/test_vfs_fs_registry.c`**

In `hosttests/cases/test_vfs_fs_registry.c`, add a test verifying that `vfs_init()` registers all 5 built-in filesystems with their Spec §5.1.1 flags and survives repeated `vfs_init()` calls without list corruption:
```c
TEST_FUNC(test_builtin_filesystems_registered_on_init) {
    /* Call vfs_init() twice to pin idempotent re-registration without cycle */
    vfs_init();
    vfs_init();

    const vfs_fs_type_t *devfs_t  = vfs_find_filesystem("devfs");
    const vfs_fs_type_t *ext2_t   = vfs_find_filesystem("ext2");
    const vfs_fs_type_t *vfat_t   = vfs_find_filesystem("vfat");
    const vfs_fs_type_t *tmpfs_t  = vfs_find_filesystem("tmpfs");
    const vfs_fs_type_t *procfs_t = vfs_find_filesystem("procfs");

    assert_not_null(devfs_t);
    assert_eq(VFS_FS_NO_DCACHE, devfs_t->fs_flags);
    assert_not_null(devfs_t->mount);

    assert_not_null(ext2_t);
    assert_eq(VFS_FS_REQUIRES_DEV, ext2_t->fs_flags);
    assert_not_null(ext2_t->mount);

    assert_not_null(vfat_t);
    assert_eq(VFS_FS_REQUIRES_DEV | VFS_FS_CASE_INSENSITIVE, vfat_t->fs_flags);
    assert_not_null(vfat_t->mount);

    assert_not_null(tmpfs_t);
    assert_eq(0u, tmpfs_t->fs_flags);
    assert_not_null(tmpfs_t->mount);

    assert_not_null(procfs_t);
    assert_eq(VFS_FS_NO_DCACHE, procfs_t->fs_flags);
    assert_not_null(procfs_t->mount);
}
```
And add `TEST_ENTRY(test_builtin_filesystems_registered_on_init),` to `TEST_LIST_BEGIN`.

- [ ] **Step 2: Run `make test-host` to verify the new test fails (RED)**

Run: `make test-host`
Expected: `test_builtin_filesystems_registered_on_init` fails with `assert_not_null(devfs_t)` failing because `vfs_init()` does not yet register the 5 built-in filesystems.

- [ ] **Step 3: Implement `*_register_fs()` and `*_fs_mount()` across the 5 filesystems and wire `vfs_init()`**

1. **In `kernel/include/fs/devfs.h` and `kernel/fs/devfs.c`**:
   - Declare `int devfs_register_fs(void);` in `kernel/include/fs/devfs.h`.
   - In `kernel/fs/devfs.c`, add:
     ```c
     static int devfs_fs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
     {
         (void)dev;
         (void)data;
         mp->ops = &devfs_ops;
         mp->fs_data = NULL;
         mp->root->ops = &devfs_ops;
         return 0;
     }

     static vfs_fs_type_t devfs_fs_type = {
         .name     = "devfs",
         .fs_flags = VFS_FS_NO_DCACHE,
         .mount    = devfs_fs_mount,
         .unmount  = NULL,
     };

     int devfs_register_fs(void)
     {
         return vfs_register_filesystem(&devfs_fs_type);
     }
     ```
   - In `devfs_init()`, call `devfs_register_fs();` (ignoring `-EEXIST`) and replace `int ret = vfs_mount("/dev", NULL, &devfs_ops, NULL);` with `int ret = vfs_mount_fs("devfs", "/dev", NULL, NULL);`.

2. **In `kernel/include/fs/tmpfs.h` and `kernel/fs/tmpfs.c`**:
   - Declare `int tmpfs_register_fs(void);` in `kernel/include/fs/tmpfs.h`.
   - In `kernel/fs/tmpfs.c`, extract the root `tmpfs_node_t` allocation into `tmpfs_fs_mount`:
     ```c
     static int tmpfs_fs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
     {
         (void)dev;
         (void)data;
         tmpfs_node_t *root = calloc(1, sizeof(tmpfs_node_t));
         if (!root) {
             debug_fs("tmpfs: root allocation failed\n");
             return -ENOMEM;
         }
         root->type = VFS_DIR;
         strcpy(root->name, "/");
         root->children = kmalloc(TMPFS_CHILDREN_INIT_CAP * sizeof(tmpfs_node_t *));
         if (!root->children) {
             kfree(root);
             debug_fs("tmpfs: children array allocation failed\n");
             return -ENOMEM;
         }
         root->child_cap = TMPFS_CHILDREN_INIT_CAP;
         root->parent    = NULL;

         mp->ops = &tmpfs_vfs_ops;
         mp->fs_data = root;
         mp->root->ops = &tmpfs_vfs_ops;
         return 0;
     }

     static vfs_fs_type_t tmpfs_fs_type = {
         .name     = "tmpfs",
         .fs_flags = 0,
         .mount    = tmpfs_fs_mount,
         .unmount  = NULL,
     };

     int tmpfs_register_fs(void)
     {
         return vfs_register_filesystem(&tmpfs_fs_type);
     }

     void tmpfs_init(void)
     {
         tmpfs_register_fs();
         if (vfs_mount_fs("tmpfs", "/tmp", NULL, NULL) == 0) {
             debug_fs("tmpfs: mounted at /tmp\n");
         }
     }
     ```
     (Include `<errno.h>` in `kernel/fs/tmpfs.c` for `-ENOMEM`.)

3. **In `kernel/include/fs/procfs.h` and `kernel/fs/procfs.c`**:
   - Declare `int procfs_register_fs(void);` in `kernel/include/fs/procfs.h`.
   - In `kernel/fs/procfs.c`, add:
     ```c
     static int procfs_fs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
     {
         (void)dev;
         (void)data;
         mp->ops = &procfs_ops;
         mp->fs_data = NULL;
         mp->root->ops = &procfs_ops;
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
     ```
   - In `procfs_init()`, call `procfs_register_fs();` and replace `int ret = vfs_mount("/proc", NULL, &procfs_ops, NULL);` with `int ret = vfs_mount_fs("procfs", "/proc", NULL, NULL);`.

4. **In `kernel/include/fs/ext2.h` and `kernel/fs/ext2.c`**:
   - Declare `int ext2_register_fs(void);` in `kernel/include/fs/ext2.h`.
   - In `kernel/fs/ext2.c`, right after `ext2_init()`, add:
     ```c
     static int ext2_fs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
     {
         (void)data;
         ext2_fs_t *fs = NULL;
         int rc = ext2_init(dev, &fs);
         if (rc != 0) {
             return rc < 0 ? rc : -EIO;
         }
         mp->ops = &ext2_vfs_ops;
         mp->fs_data = fs;
         mp->root->ops = &ext2_vfs_ops;
         return 0;
     }

     static vfs_fs_type_t ext2_fs_type = {
         .name     = "ext2",
         .fs_flags = VFS_FS_REQUIRES_DEV,
         .mount    = ext2_fs_mount,
         .unmount  = NULL,
     };

     int ext2_register_fs(void)
     {
         return vfs_register_filesystem(&ext2_fs_type);
     }
     ```

5. **In `kernel/include/fs/fat.h` and `kernel/fs/fat.c`**:
   - Declare `int fat_register_fs(void);` in `kernel/include/fs/fat.h`.
   - In `kernel/fs/fat.c`, right after `fat32_init()`, add:
     ```c
     static int fat_fs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
     {
         (void)data;
         fat32_fs_t *fs = NULL;
         int rc = fat32_init(dev, &fs);
         if (rc != 0) {
             return rc < 0 ? rc : -EIO;
         }
         mp->ops = &fat_vfs_ops;
         mp->fs_data = fs;
         mp->root->ops = &fat_vfs_ops;
         return 0;
     }

     static vfs_fs_type_t fat_fs_type = {
         .name     = "vfat",
         .fs_flags = VFS_FS_REQUIRES_DEV | VFS_FS_CASE_INSENSITIVE,
         .mount    = fat_fs_mount,
         .unmount  = NULL,
     };

     int fat_register_fs(void)
     {
         return vfs_register_filesystem(&fat_fs_type);
     }
     ```

6. **In `kernel/fs/vfs.c`**:
   - Include `<fs/devfs.h>`, `<fs/ext2.h>`, `<fs/fat.h>`, `<fs/tmpfs.h>`, `<fs/procfs.h>`.
   - In `vfs_init()`, after setting `vfs_initialized = 1;`, call:
     ```c
     devfs_register_fs();
     ext2_register_fs();
     fat_register_fs();
     tmpfs_register_fs();
     procfs_register_fs();
     ```

7. **In `hosttests/cases/test_devfs_input_dispatch.c`**:
   - Update the VFS mount stub at lines 90–100 so `devfs_init()` (which now calls `devfs_register_fs()` and `vfs_mount_fs("devfs", "/dev", NULL, NULL)`) populates `devfs_mount_ops`:
     ```c
     static vfs_ops_t *devfs_mount_ops;
     static vfs_fs_type_t *s_registered_devfs_type;
     int vfs_register_filesystem(vfs_fs_type_t *fs_type)
     {
         s_registered_devfs_type = fs_type;
         return 0;
     }
     int vfs_mount_fs(const char *type_name, const char *path,
                      block_device_t *dev, void *data)
     {
         (void)type_name;
         vfs_node_t dummy_root;
         vfs_mount_t dummy_mp;
         memset(&dummy_root, 0, sizeof(dummy_root));
         memset(&dummy_mp, 0, sizeof(dummy_mp));
         dummy_mp.path = path;
         dummy_mp.root = &dummy_root;
         if (s_registered_devfs_type && s_registered_devfs_type->mount) {
             int rc = s_registered_devfs_type->mount(&dummy_mp, dev, data);
             if (rc < 0) return rc;
             devfs_mount_ops = dummy_mp.ops;
         }
         return 0;
     }
     int vfs_mount(const char *path, block_device_t *dev, vfs_ops_t *ops,
                   void *fs_data)
     {
         (void)path; (void)dev; (void)fs_data;
         devfs_mount_ops = ops;
         return 0;
     }
     ```

8. **In `hosttests/Makefile` and `hosttests/cases/test_vfs_fs_registry.c`**:
   - In `hosttests/Makefile`, add object rules for `ext2_production.o`, `fat_production.o`, and `tmpfs_production.o` and link them alongside `$(TEST_BLD)/gfx_devfs_production.o` into `$(TEST_BLD)/test_vfs_fs_registry.elf`:
     ```makefile
     $(TEST_BLD)/ext2_production.o: $(TESTS_DIR)/kernel/fs/ext2.c \
     		$(TESTS_DIR)/kernel/include/fs/ext2.h \
     		$(TESTS_DIR)/kernel/include/fs/vfs.h \
     		$(TEST_MOCK)/vfs_test_runtime.h
     	@mkdir -p $(TEST_BLD)
     	$(HOST_CC) $(VFS_HOST_CFLAGS) -c $< -o $@

     $(TEST_BLD)/fat_production.o: $(TESTS_DIR)/kernel/fs/fat.c \
     		$(TESTS_DIR)/kernel/include/fs/fat.h \
     		$(TESTS_DIR)/kernel/include/fs/vfs.h \
     		$(TEST_MOCK)/vfs_test_runtime.h
     	@mkdir -p $(TEST_BLD)
     	$(HOST_CC) $(VFS_HOST_CFLAGS) -c $< -o $@

     $(TEST_BLD)/tmpfs_production.o: $(TESTS_DIR)/kernel/fs/tmpfs.c \
     		$(TESTS_DIR)/kernel/include/fs/tmpfs.h \
     		$(TESTS_DIR)/kernel/include/fs/vfs.h \
     		$(TEST_MOCK)/vfs_test_runtime.h
     	@mkdir -p $(TEST_BLD)
     	$(HOST_CC) $(VFS_HOST_CFLAGS) -c $< -o $@

     $(TEST_BLD)/test_vfs_fs_registry.elf: $(TEST_BLD)/test_vfs_fs_registry.o \
     		$(TEST_BLD)/vfs_production.o \
     		$(TEST_BLD)/ext2_production.o \
     		$(TEST_BLD)/fat_production.o \
     		$(TEST_BLD)/tmpfs_production.o \
     		$(TEST_BLD)/gfx_devfs_production.o \
     		$(TEST_BLD)/rwlock.o \
     		$(LIBC_OBJS) $(MOCK_OBJS)
     	$(HOST_CC) -Wl,--gc-sections -o $@ $^
     ```
   - In `hosttests/cases/test_vfs_fs_registry.c`, add the narrow link stubs needed by `ext2.c`, `fat.c`, `tmpfs.c`, and `procfs_register_fs()` (since `procfs.c` depends on `vma.h`/`pty.h`):
     ```c
     int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
     { (void)dev; (void)lba; (void)count; (void)buf; return -EIO; }
     int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
     { (void)dev; (void)lba; (void)count; (void)buf; return -EIO; }
     uint64_t alloc_4k_page(void) { return (uint64_t)(uintptr_t)calloc(1, 4096); }
     void free_4k_page(uint64_t addr) { free((void *)(uintptr_t)addr); }

     static int host_procfs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
     { (void)dev; (void)data; mp->ops = &s_dummy_ops; mp->root->ops = &s_dummy_ops; return 0; }
     static vfs_fs_type_t s_host_procfs_type = {
         .name     = "procfs",
         .fs_flags = VFS_FS_NO_DCACHE,
         .mount    = host_procfs_mount,
     };
     int procfs_register_fs(void) { return vfs_register_filesystem(&s_host_procfs_type); }
     ```

- [ ] **Step 4: Run `make test-host` to verify Task 2 passes (GREEN)**

Run: `make test-host`
Expected: `test_vfs_fs_registry.elf` (all 5 tests) and `test_devfs_input_dispatch.elf` pass; entire host test suite passes.

- [ ] **Step 5: Commit Task 2**

```bash
git add kernel/include/fs/devfs.h kernel/fs/devfs.c \
        kernel/include/fs/tmpfs.h kernel/fs/tmpfs.c \
        kernel/include/fs/procfs.h kernel/fs/procfs.c \
        kernel/include/fs/ext2.h kernel/fs/ext2.c \
        kernel/include/fs/fat.h kernel/fs/fat.c \
        kernel/fs/vfs.c \
        hosttests/cases/test_devfs_input_dispatch.c \
        hosttests/cases/test_vfs_fs_registry.c \
        hosttests/Makefile
git commit -m "feat(fs): register devfs/ext2/vfat/tmpfs/procfs in VFS registry (ARCH-5 Phase 1)"
```

---

### Task 3: Migrate `kernel/fs/boot.c` to `vfs_mount_fs` & Run Phase 1 Verification Gates

**Files:**
- Modify: `kernel/fs/boot.c:71-110`
- Modify: `hosttests/cases/test_device_boot.c:78-125,525-601`

**Interfaces:**
- Consumes: `vfs_mount_fs("ext2" | "vfat", path, dev, NULL)` from Tasks 1–2.
- Produces: `fs_boot_mounts()` in `kernel/fs/boot.c` mounting root and `/boot` via `vfs_mount_fs()` without referencing `ext2_vfs_ops`, `fat_vfs_ops`, `ext2_init`, or `fat32_init`.

- [ ] **Step 1: Update `hosttests/cases/test_device_boot.c` to assert `vfs_mount_fs` calls and `/boot` failure tolerance**

In `hosttests/cases/test_device_boot.c`:
1. Add `vfs_mount_fs` mock that records the mounted filesystem type and path and delegates to `s_mock_ext2_init_ret` / `s_mock_fat32_init_ret`:
   ```c
   static char s_root_fs_type[16];
   static char s_boot_fs_type[16];

   int vfs_mount_fs(const char *type_name, const char *path,
                    block_device_t *dev, void *data)
   {
       (void)dev;
       (void)data;
       if (!type_name || !path) return -EINVAL;
       if (strcmp(type_name, "ext2") == 0) {
           if (s_mock_ext2_init_ret != 0) return s_mock_ext2_init_ret;
       } else if (strcmp(type_name, "vfat") == 0) {
           if (s_mock_fat32_init_ret != 0) return s_mock_fat32_init_ret;
       } else {
           return -ENODEV;
       }
       if (strcmp(path, "/") == 0) {
           s_root_mounted = true;
           strncpy(s_root_fs_type, type_name, sizeof(s_root_fs_type) - 1);
       } else if (strcmp(path, "/boot") == 0) {
           s_boot_mounted = true;
           strncpy(s_boot_fs_type, type_name, sizeof(s_boot_fs_type) - 1);
       }
       return 0;
   }
   ```
   Also clear `s_root_fs_type[0] = '\0'; s_boot_fs_type[0] = '\0';` in `reset_test_state()`.
2. In `test_root_requirement()`:
   - In Subtest B (GPT layout): assert `assert_str_eq("ext2", s_root_fs_type);` and `assert_str_eq("vfat", s_boot_fs_type);`.
   - In Subtest C (fallback single-FAT32): assert `assert_str_eq("vfat", s_root_fs_type);`.
   - Add Subtest E: GPT present, `s_mock_ext2_init_ret = 0`, `s_mock_fat32_init_ret = -EIO` (`/boot` mount fails while `/` succeeds) → `fs_boot_mounts()` does NOT panic, `s_root_mounted` is `true`, `s_boot_mounted` is `false`, `s_tmpfs_init_called` and `s_procfs_init_called` are `true`.

- [ ] **Step 2: Run `make test-host` to verify `test_device_boot` fails before `boot.c` is updated (RED)**

Run: `make test-host`
Expected: `test_device_boot.elf` fails on `assert_str_eq("ext2", s_root_fs_type)` because `kernel/fs/boot.c` still calls `vfs_mount` instead of `vfs_mount_fs`.

- [ ] **Step 3: Update `mount_partitioned_disk()` in `kernel/fs/boot.c` to use `vfs_mount_fs()`**

In `kernel/fs/boot.c`:
- Remove `#include <fs/fat.h>` and `#include <fs/ext2.h>` if no longer needed.
- Update `mount_partitioned_disk()`:
  ```c
  static bool mount_partitioned_disk(void)
  {
      block_device_t *disk = find_first_disk();
      if (!disk) return false;

      gpt_info_t *gpt = gpt_scan(disk);
      if (!gpt) {
          // Fallback: old single-FAT32 layout (whole disk is FAT32).
          if (vfs_mount_fs("vfat", "/", disk, NULL) == 0) {
              return true;
          }
          return false;
      }

      // Dual-partition layout:
      //   gpt->partitions[0] = hda1 (FAT32 ESP) → /boot
      //   gpt->partitions[1] = hda2 (ext2)      → /
      if (gpt->count < 2) return false;

      bool root_ok = false;

      if (vfs_mount_fs("ext2", "/", gpt->partitions[1].dev, NULL) == 0) {
          root_ok = true;
      } else {
          log_err("EXT2: mount failed — / not available\n");
      }

      if (vfs_mount_fs("vfat", "/boot", gpt->partitions[0].dev, NULL) != 0) {
          log_warn("FAT32: /boot mount failed\n");
      }

      return root_ok;
  }
  ```

- [ ] **Step 4: Run all Phase 1 verification gates (`make clean` + host + static + dual-arch + QEMU suites)**

Run the mandatory clean and verification sequence from Spec §7:
```bash
make clean
make test-host
make test-static
make PROFILE=x86_64-clang kernel
make PROFILE=aarch64-clang kernel
make test-qemu SUITE=phase-0
make KERNEL_SELFTEST=1 test-kernel-selftest
make OS01_SYSTEST=1 test-syscall
```
Expected:
- `make test-host`: all host tests pass (including `test_vfs_fs_registry.elf`, `test_device_boot.elf`, `test_devfs_input_dispatch.elf`).
- `make test-static`: all static audits pass.
- `make PROFILE=x86_64-clang kernel` and `make PROFILE=aarch64-clang kernel`: zero warnings, zero errors.
- `make test-qemu SUITE=phase-0`: PASS.
- `make KERNEL_SELFTEST=1 test-kernel-selftest`: PASS (`ext2_selftest_*`, `tmpfs_selftest_*`, `symlink_selftest` all pass).
- `make OS01_SYSTEST=1 test-syscall`: PASS (228 syscall tests pass).

- [ ] **Step 5: Commit Task 3**

```bash
git add kernel/fs/boot.c hosttests/cases/test_device_boot.c
git commit -m "feat(boot): switch boot filesystem mounting to vfs_mount_fs (ARCH-5 Phase 1)"
```
