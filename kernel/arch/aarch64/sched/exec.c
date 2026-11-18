/* kernel/arch/aarch64/sched/exec.c — VFS-backed user program launch.
 *
 * The aarch64 counterpart of kernel/arch/x86_64/sched/exec.c
 * (spawn_user_task) and the product-side arch_syscall_exec path.
 *
 * The image machinery (kernel/arch/aarch64/memory/user_image.c) builds
 * the private address space out of a memory-resident ET_EXEC buffer;
 * this file owns the VFS half: resolve the path, read the ELF bytes,
 * and hand them to the staged-failure construction that ends in a
 * single publication point (sched_alloc_and_publish_task).  The ELF
 * buffer is transient — aarch64_user_image_load copies every segment
 * page into mm-owned pages, so the buffer is released as soon as the
 * load returns.
 *
 * The staged-failure shape mirrors the x86 implementation: every
 * fallible preparation runs PRIVATELY before the task becomes visible,
 * and each stage releases exactly the resources it acquired.
 */

#include <arch/aarch64/exec.h>
#include <arch/aarch64/user_image.h>
#include <arch/aarch64/user_task.h>
#include <arch/aarch64/user_mm.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/aarch64/user_vm.h>
#include <arch/cpu.h>
#include <core/debug.h>
#include <errno.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/slab.h>
#include <sched/boot.h>
#include <sched/internal.h>
#include <sched/task.h>
#include <string.h>

/* Read the whole file behind `node` into a kmalloc'd buffer.  Returns
 * 0 with *out/*out_size on success (caller owns the buffer), a
 * negative errno otherwise.  The node reference stays with the
 * caller.  Shared by spawn_user_task and the product arch_syscall_exec
 * path (cpu/user_task.c). */
int aarch64_exec_read_elf(vfs_node_t *node, void **out, size_t *out_size)
{
    if (!node || node->type != VFS_FILE)
        return -ENOEXEC;
    if (node->size == 0 || node->size > AARCH64_SPAWN_ELF_MAX_BYTES)
        return -ENOEXEC;

    size_t size = (size_t)node->size;
    void *buf = kmalloc(size);
    if (!buf)
        return -ENOMEM;

    uint64_t off = 0;
    while (off < size) {
        int n = vfs_read(node, off, size - off, (char *)buf + off);
        if (n <= 0) {
            kfree(buf);
            return (n < 0) ? (int)n : -EIO;
        }
        off += (uint64_t)n;
    }
    *out = buf;
    *out_size = size;
    return 0;
}

int64_t spawn_user_task(const char *path, const char *const *argv)
{
    if (!path || path[0] == '\0')
        return -ENOENT;

    /* Single-core runtime gate: the user-mm primitives refuse a
     * multi-core or non-BSP configuration. */
    int rc = aarch64_user_mm_runtime_check();
    if (rc)
        return rc;

    /* The scheduler must be ready: a user task is published into a
     * live runqueue (the boot-5 creation window keeps the scheduler
     * not-yet-ok while allowing CPU0 publication). */
    if (!__atomic_load_n(&percpu_data[0].scheduler_ok, __ATOMIC_ACQUIRE)) {
        if (!sched_boot_creation_open() || cpu_id() != 0)
            return -EAGAIN;
    }

    /* 1. Resolve the ELF through VFS.  The node reference is released
     * before the image construction — the bytes buffer carries all the
     * data the loader needs. */
    vfs_node_t *node = vfs_lookup(path);
    if (!node) {
        debug_task("spawn: cannot open '%s'\n", path);
        return -ENOENT;
    }

    void *bytes = NULL;
    size_t size = 0;
    rc = aarch64_exec_read_elf(node, &bytes, &size);
    vfs_node_put(node);
    node = NULL;
    if (rc) {
        debug_task("spawn: ELF read failed for '%s' rc=%d\n", path, rc);
        return rc;
    }

    /* 2. Build the image (ELF validation + mm-owned pages + stack). */
    struct aarch64_user_image *image = NULL;
    rc = aarch64_user_image_load(bytes, size,
                                 AARCH64_PRODUCT_STACK_PAGES, &image);
    kfree(bytes);
    bytes = NULL;
    if (rc)
        return rc;

    /* 3. Initial stack: argv (argv[0] = program path), envp NULL,
     * auxv.  The fd table the task inherits is a duplicate of the
     * creator's (the boot idle task's /dev/tty stdio triple). */
    rc = aarch64_user_image_setup_stack(image, argv, NULL,
                                        AARCH64_PRODUCT_STACK_PAGES);
    if (rc)
        goto fail_image;

    /* The creator runs on the boot stack, where `current` is not a
     * task (the aarch64 handoff explicitly avoids deriving current
     * there); the boot-5 stdio transaction installs the fds on
     * &init_task_union.task — the same task arch_sched_boot_prepare
     * publishes as CPU0 idle. */
    task_t *creator = percpu_data[0].idle;
    files_t *files = creator && creator->files ? files_dup(creator->files)
                                               : NULL;
    if (creator && creator->files && !files) {
        rc = -ENOMEM;
        goto fail_image;
    }

    /* 4. The owner record; on failure the image rollback below owns
     * everything. */
    struct aarch64_user_task *owner = kmalloc(sizeof(*owner));
    if (!owner) {
        if (files)
            files_unpin(files);
        rc = -ENOMEM;
        goto fail_image;
    }
    owner->image = image;
    owner->catalog = NULL;      /* VFS-spawned: exec re-reads from VFS */
    owner->catalog_count = 0;
    owner->catalog_index = 0;

    int files_state = 0;
    struct user_task_prepare_ctx pctx = {
        .owner  = owner,
        .parent = NULL,         /* resident BSP idle parents the spawn */
        .frame  = NULL,         /* fresh image frame */
        .files  = files,
        .files_state = &files_state,
    };

    task_t *tsk = NULL;
    int pid = 0;
    rc = sched_alloc_and_publish_task(0, true /* pinned to the BSP */,
                                      PF_PROCESS, task_basename(path),
                                      user_task_prepare, user_task_discard,
                                      &pctx, &tsk, &pid);
    if (rc) {
        /* user_task_discard released the fd table iff prepare ran
         * (files_state); the image and the owner record are still
         * ours. */
        if (files && files_state == 0)
            files_unpin(files);
        kfree(owner);
        goto fail_image;
    }
    return pid;

fail_image:
    aarch64_user_image_destroy(image);
    return rc;
}
