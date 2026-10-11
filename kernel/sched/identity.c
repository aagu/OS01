/*
 * kernel/sched/identity.c — task identity fields (monitor plan Task 4,
 * spec §5.1): birth_id, start_ns, runtime_ticks, comm.
 *
 * Split out of core.c/fork.c so the lifecycle helpers are unit-testable
 * on the host (hosttests CASE=procfs drives this exact file); fork.c /
 * exec.c call into it on their existing paths.
 *
 * Protocol (spec §5.1):
 *   - spawn/fork assign birth_id + start_ns BEFORE the task is
 *     published to the task table, and runtime_ticks starts at zero —
 *     a child never inherits the parent's runtime, birth or start time;
 *   - fork copies the parent's comm under task_list_lock (task_list_lock
 *     is the comm read/write protocol, shared with the procfs snapshot
 *     copy in procfs_snap.c);
 *   - exec success renames comm to the new image's basename under
 *     task_list_lock; every exec failure path returns long before the
 *     rename, so the old name survives.  exec is NOT a new process:
 *     birth_id/start_ns/runtime_ticks are untouched.
 *
 * Nothing here formats or allocates under the lock — comm writes are a
 * fixed-size copy into a NUL-terminated 64-byte field.
 */

#include <sched/task.h>
#include <arch/clocksource.h>
#include <arch/spinlock.h>

#include <errno.h>
#include <string.h>

/* Global birth counter — atomic because spawn/fork may race across
 * CPUs (same discipline as alloc_pid's pid_counter). */
static volatile uint64_t birth_id_counter = 0;

uint64_t task_next_birth_id(void)
{
    return __atomic_fetch_add(&birth_id_counter, 1, __ATOMIC_RELAXED) + 1;
}

const char *task_basename(const char *path)
{
    if (!path)
        return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Set comm.  Caller holds task_list_lock (or owns the task exclusively
 * pre-publication).  Always NUL-terminates; names longer than 63 bytes
 * are truncated. */
void task_comm_set(task_t *tsk, const char *name)
{
    if (!tsk)
        return;
    if (!name)
        name = "";
    size_t i;
    for (i = 0; i + 1 < sizeof(tsk->comm) && name[i]; i++)
        tsk->comm[i] = name[i];
    tsk->comm[i] = '\0';
}

/* fork: the child copies ONLY the parent's comm — never the parent's
 * runtime, birth_id or start_ns.  Caller holds task_list_lock so the
 * copy is serialized against procfs snapshot readers (spec §5.1). */
void task_comm_inherit(task_t *child, const task_t *parent)
{
    if (!child || !parent)
        return;
    memcpy(child->comm, parent->comm, sizeof(child->comm));
    child->comm[sizeof(child->comm) - 1] = '\0';
}

/* Pre-publication identity init: birth_id, start_ns, zero runtime, and
 * the initial comm (NULL → empty; callers with a path pass the result
 * of task_basename()).  Must run before the task enters the task table. */
void task_identity_init(task_t *tsk, const char *name)
{
    if (!tsk)
        return;
    tsk->birth_id      = task_next_birth_id();
    tsk->start_ns      = arch_clocksource_read_ns();
    tsk->runtime_ticks = 0;
    task_comm_set(tsk, name);
}

/* exec commit: rename to basename(path) under task_list_lock.  The
 * basename is copied to the stack BEFORE the lock so no caller-supplied
 * (possibly user) pointer is dereferenced with IRQs off.  Returns 0 or
 * -EINVAL; on failure comm is untouched. */
int task_comm_apply_exec(task_t *tsk, const char *path)
{
    if (!tsk || !path)
        return -EINVAL;
    const char *base = task_basename(path);
    if (!*base)
        return -EINVAL;

    char local[sizeof(tsk->comm)];
    size_t i;
    for (i = 0; i + 1 < sizeof(local) && base[i]; i++)
        local[i] = base[i];
    local[i] = '\0';

    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    task_comm_set(tsk, local);
    spin_unlock_irqrestore(&task_list_lock, flags);
    return 0;
}
