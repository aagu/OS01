/*
 * hosttests/mock/nofile_soft_stub.c
 *
 * ARCH-10 Task 6 — the RLIMIT_NOFILE soft-limit snapshot for the host
 * harnesses that compile kernel/fs/file.c against a REDUCED mock
 * task_t (hosttests/mock/poll_test_runtime.h and
 * hosttests/mock/gfx_test_runtime.h).
 *
 * kernel/fs/file.c reads the installing task's NOFILE soft limit
 * through task_nofile_soft() (declared by <sched/resource.h>).  The
 * production implementation (kernel/sched/resource.c) reads
 * tsk->nofile_soft / tsk->nofile_hard / tsk->nofile_lock — fields
 * the reduced mock task_t does not have — so it cannot be compiled
 * against the mock.  This stub supplies the snapshot instead.
 *
 * The mock fd array holds OS01_NOFILE_CAPACITY descriptors, so the
 * limit is the full capacity and the install behaviour of those
 * harnesses matches the pre-ARCH-10 one (their assertions are about
 * the poll / devfs / PTY protocols, not about the limit).
 *
 * The argument is ignored on purpose: the mock fd table is not owned
 * by a mock task, so there is no per-task limit to read.
 *
 * EVERY binary that links a kernel/fs/file.c object compiled against
 * one of those mock runtimes MUST link this stub — the symbol is the
 * only thing that keeps the link independent of --gc-sections.
 */

#include <stdint.h>

#include <config/limits.h>

struct task_struct;

uint64_t task_nofile_soft(struct task_struct *t)
{
    (void)t;
    return OS01_NOFILE_CAPACITY;
}
