#include <kernel/deferred_free.h>
#include <kernel/slab.h>
#include <kernel/task.h>
#include <kernel/file.h>
#include <kernel.h>                  // container_of
#include <kernel/arch/x86_64/spinlock.h>
#include <stdbool.h>

// ── Queue state ──────────────────────────────────────────────
static spinlock_T  df_lock = { .lock = 1L };
static list_t      df_queue;
static task_t     *df_kthread;

// ── Thin wrappers ────────────────────────────────────────────
// Avoid UB from casting kfree (returns size_t) to
// deferred_fn_t (void (*)(void*)). Both calling conventions are
// identical on x86_64 SysV, but the C standard forbids the cast.
static void kfree_wrapper(void *p)       { kfree(p); }
static void files_free_wrapper(void *p)  { files_free((files_t *)p); }

// ── Blocker condition —────────────────────────────────────────
// Reads df_queue.next/prev without df_lock — intentional; on x86_64
// aligned pointer reads are atomic, and a stale "empty" read at
// worst causes one extra blocker_wait cycle (benign).
static bool df_queue_has_work(task_t *self)
{
    (void)self;
    return !list_is_empty(&df_queue);
}

// ── Public API ────────────────────────────────────────────────

void deferred_free(deferred_fn_t fn, void *ptr)
{
    deferred_work_t *w = kmalloc(sizeof(*w));
    if (!w) return;  // OOM: leak the ptr rather than panic
    list_init(&w->node);
    w->fn  = fn;
    w->ptr = ptr;

    uint64_t flags = spin_lock_irqsave(&df_lock);
    list_add_to_before(&df_queue, &w->node);
    spin_unlock_irqrestore(&df_lock, flags);

    if (df_kthread)
        blocker_wake(df_kthread);
}

// ── Convenience API —─────────────────────────────────────────
// Thin wrappers that keep kfree_wrapper/files_free_wrapper
// static while providing a clean interface for callers.

void deferred_kfree(void *ptr)
{
    deferred_free(kfree_wrapper, ptr);
}

void deferred_files_free(files_t *fs)
{
    deferred_free(files_free_wrapper, fs);
}

// ── Reaper main loop ──────────────────────────────────────────

static uint64_t df_reaper_main(uint64_t arg)
{
    (void)arg;
    for (;;) {
        blocker_wait(df_queue_has_work, BLOCKER_DEFERRED_FREE, false);

        uint64_t flags = spin_lock_irqsave(&df_lock);
        while (!list_is_empty(&df_queue)) {
            deferred_work_t *w =
                container_of(df_queue.next, deferred_work_t, node);
            list_del(&w->node);
            spin_unlock_irqrestore(&df_lock, flags);

            w->fn(w->ptr);       // e.g. kfree_wrapper(stack_alloc_base)
            kfree(w);            // free the work item itself

            flags = spin_lock_irqsave(&df_lock);
        }
        spin_unlock_irqrestore(&df_lock, flags);
    }
    return 0;
}

// ── Initialization ────────────────────────────────────────────

task_t *deferred_free_spawn(void)
{
    list_init(&df_queue);
    df_kthread = create_kthread(df_reaper_main, 0, "df-reaper");
    return df_kthread;
}
