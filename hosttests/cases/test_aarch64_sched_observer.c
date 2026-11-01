/* test_aarch64_sched_observer.c — P1 Task 2: the REAL
 * kernel/selftest/aarch64_sched_observer.c under genuine host
 * concurrency (pthread).
 *
 * Cases (plan P1 Task 2 step 1):
 *   1. two CPUs allocate/free concurrently — zero lost records, the
 *      batch ends clean, created/released counts match exactly.
 *   2. a double free_complete is a recorded failure.
 *   3. an address reused across generations is never confused: the
 *      new record gets a fresh token, and a stale token can neither
 *      complete nor free the new record.
 *   4. exhausting the 512 live slots records an overflow and fails
 *      the batch.
 *   5. end fails while any record is ALLOCATED (live != 0).
 *   6. end fails while a record is FREEING (release in flight), and
 *      succeeds only after the real release completes.
 *   7. the legacy alloc=false call shape is now a batch failure.
 *   8. untracked pointers free_begin to token 0 and token 0 completes
 *      as a no-op.
 */

#include "test_framework.h"
#include "../mock/aarch64_sched_observer_runtime.h"
#include <selftest/aarch64_sched_observer.h>
#include <pthread.h>

/* The mock's spin surface resolves to these. */
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
void aarch64_sched_observer_host_lock(void) { pthread_mutex_lock(&host_lock); }
void aarch64_sched_observer_host_unlock(void) { pthread_mutex_unlock(&host_lock); }
arch_irq_state_t arch_local_irq_save(void) { return 0; }
void arch_local_irq_restore(arch_irq_state_t s) { (void)s; }

#define KIND_STACK AARCH64_SCHED_RESOURCE_STACK
#define KIND_THREAD AARCH64_SCHED_RESOURCE_THREAD

/* ── 1. concurrent alloc/free from two CPUs ────────────────────── */
#define CONC_ITER 400
static void *conc_worker(void *arg)
{
    unsigned cpu = (unsigned)(uintptr_t)arg;
    for (int i = 0; i < CONC_ITER; ++i) {
        void *p = malloc(64);           /* stand-in raw stack */
        aarch64_sched_observe_resource(p, KIND_STACK, true);
        uint64_t tok = aarch64_sched_observe_free_begin(p, KIND_STACK);
        free(p);                        /* the REAL release */
        aarch64_sched_observe_free_complete(tok);
        if ((i & 63) == 0) {
            /* Cross-kind interleaving from both CPUs. */
            void *c = malloc(32);
            aarch64_sched_observe_resource(c, KIND_THREAD, true);
            uint64_t ctok = aarch64_sched_observe_free_begin(c, KIND_THREAD);
            free(c);
            aarch64_sched_observe_free_complete(ctok);
        }
        (void)cpu;
    }
    return NULL;
}

static void suite_concurrent_alloc_free(void)
{
    TEST_SUITE("two CPUs allocate and release concurrently");
    pthread_t a, b;
    aarch64_sched_observer_begin(0, 1);
    pthread_create(&a, NULL, conc_worker, (void *)(uintptr_t)0);
    pthread_create(&b, NULL, conc_worker, (void *)(uintptr_t)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    uint64_t created_before = aarch64_sched_observer_created();
    assert_eq(0, aarch64_sched_observer_end(0, 1));
    /* 2 threads × (CONC_ITER stack + one cross-kind alloc every 64th
     * iteration, i.e. i&63==0 → (CONC_ITER+63)/64 of them). */
    assert_eq(2 * (CONC_ITER + (CONC_ITER + 63) / 64), (int)created_before);
    assert_eq(created_before,
              (int)aarch64_sched_observer_released(KIND_STACK) +
              (int)aarch64_sched_observer_released(KIND_THREAD));
}

/* ── 2. duplicate completion ───────────────────────────────────── */
static void suite_duplicate_complete(void)
{
    TEST_SUITE("a double free_complete fails the batch");
    aarch64_sched_observer_begin(0, 2);
    void *p = malloc(16);
    aarch64_sched_observe_resource(p, KIND_STACK, true);
    uint64_t tok = aarch64_sched_observe_free_begin(p, KIND_STACK);
    assert_true(tok != 0);
    free(p);
    aarch64_sched_observe_free_complete(tok);
    aarch64_sched_observe_free_complete(tok);       /* second completion */
    assert_true(aarch64_sched_observer_end(0, 2) != 0);
}

/* ── 3. cross-generation address reuse ─────────────────────────── */
static void suite_address_reuse(void)
{
    TEST_SUITE("a reused address keeps its generations distinct");
    static uint8_t backing[64];
    void *p = (void *)backing;

    aarch64_sched_observer_begin(0, 3);
    aarch64_sched_observe_resource(p, KIND_STACK, true);
    uint64_t t1 = aarch64_sched_observe_free_begin(p, KIND_STACK);
    assert_true(t1 != 0);
    aarch64_sched_observe_free_complete(t1);
    assert_eq(0, aarch64_sched_observer_end(0, 3));

    /* New generation: same address allocates a FRESH token. */
    aarch64_sched_observer_begin(1, 4);
    aarch64_sched_observe_resource(p, KIND_STACK, true);
    uint64_t t2 = aarch64_sched_observe_free_begin(p, KIND_STACK);
    assert_true(t2 != 0);
    assert_true(t2 != t1);

    /* The stale token must neither complete (it was already FREED in
     * gen 3, so its slot is gone) nor consume the new record: t2 must
     * still be completable afterwards. */
    uint64_t rel_before = aarch64_sched_observer_released(KIND_STACK);
    aarch64_sched_observe_free_complete(t1);
    assert_true(aarch64_sched_observer_end(1, 4) != 0);  /* recorded */
    aarch64_sched_observe_free_complete(t2);
    assert_eq(rel_before + 1,
              (int)aarch64_sched_observer_released(KIND_STACK));
    aarch64_sched_observe_free_complete(t2);   /* duplicate completion */
    assert_true(aarch64_sched_observer_end(1, 4) != 0);
}

/* ── 4. capacity exhaustion ────────────────────────────────────── */
static void suite_capacity_overflow(void)
{
    TEST_SUITE("exhausting 512 live slots records an overflow");
    static void *ptrs[AARCH64_SCHED_OBSERVER_CAPACITY + 1];

    /* A fully released batch of exactly CAPACITY records ends clean. */
    aarch64_sched_observer_begin(0, 5);
    for (unsigned i = 0; i < AARCH64_SCHED_OBSERVER_CAPACITY; ++i) {
        ptrs[i] = malloc(8);
        aarch64_sched_observe_resource(ptrs[i], KIND_STACK, true);
    }
    for (unsigned i = 0; i < AARCH64_SCHED_OBSERVER_CAPACITY; ++i) {
        uint64_t tok = aarch64_sched_observe_free_begin(ptrs[i], KIND_STACK);
        assert_true(tok != 0);
        free(ptrs[i]);
        aarch64_sched_observe_free_complete(tok);
    }
    assert_eq(0, aarch64_sched_observer_end(0, 5));

    /* Holding all CAPACITY records live makes the next allocation an
     * overflow: the 513th record cannot be tracked and the batch
     * cannot end clean.  (The phase-1 pointers are already freed on
     * the host side — the observer tracks the values, and phase 2's
     * release bookkeeping below must not free() them again.) */
    aarch64_sched_observer_begin(0, 6);
    for (unsigned i = 0; i < AARCH64_SCHED_OBSERVER_CAPACITY; ++i)
        aarch64_sched_observe_resource(ptrs[i], KIND_STACK, true);
    ptrs[AARCH64_SCHED_OBSERVER_CAPACITY] = malloc(8);
    aarch64_sched_observe_resource(ptrs[AARCH64_SCHED_OBSERVER_CAPACITY],
                                   KIND_STACK, true);   /* one too many */
    assert_true(aarch64_sched_observer_end(0, 6) != 0);
    /* Release bookkeeping so later suites start from a drained table
     * (no host-side free: those blocks were released in phase 1). */
    for (unsigned i = 0; i < AARCH64_SCHED_OBSERVER_CAPACITY; ++i) {
        uint64_t tok = aarch64_sched_observe_free_begin(ptrs[i], KIND_STACK);
        assert_true(tok != 0);
        aarch64_sched_observe_free_complete(tok);
    }
    free(ptrs[AARCH64_SCHED_OBSERVER_CAPACITY]);
}

/* ── 5-6. end gates ────────────────────────────────────────────── */
static void suite_end_gates(void)
{
    TEST_SUITE("end fails on live records and on in-flight releases");
    aarch64_sched_observer_begin(0, 7);
    void *p = malloc(8);
    aarch64_sched_observe_resource(p, KIND_STACK, true);
    assert_true(aarch64_sched_observer_end(0, 7) != 0);  /* ALLOCATED */
    uint64_t tok = aarch64_sched_observe_free_begin(p, KIND_STACK);
    assert_true(aarch64_sched_observer_end(0, 7) != 0);  /* FREEING */
    free(p);
    aarch64_sched_observe_free_complete(tok);
    assert_eq(0, aarch64_sched_observer_end(0, 7));      /* now clean */
}

/* ── 7. the legacy alloc=false call is a failure ───────────────── */
static void suite_legacy_false_alloc(void)
{
    TEST_SUITE("the legacy alloc=false call shape is a batch failure");
    aarch64_sched_observer_begin(0, 8);
    void *p = malloc(8);
    aarch64_sched_observe_resource(p, KIND_STACK, true);
    aarch64_sched_observe_resource(p, KIND_STACK, false);  /* FORBIDDEN */
    uint64_t tok = aarch64_sched_observe_free_begin(p, KIND_STACK);
    free(p);
    aarch64_sched_observe_free_complete(tok);
    assert_true(aarch64_sched_observer_end(0, 8) != 0);
}

/* ── 8. untracked pointers ─────────────────────────────────────── */
static void suite_untracked(void)
{
    TEST_SUITE("untracked pointers free_begin to 0 and complete as no-op");
    aarch64_sched_observer_begin(0, 9);
    void *p = malloc(8);
    assert_eq(0, (int)aarch64_sched_observe_free_begin(p, KIND_STACK));
    aarch64_sched_observe_free_complete(0);            /* no-op */
    aarch64_sched_observe_free_complete(0xDEADBEEF);   /* unknown token */
    assert_true(aarch64_sched_observer_end(0, 9) != 0);
    free(p);
}

int main(void)
{
    suite_concurrent_alloc_free();
    suite_duplicate_complete();
    suite_address_reuse();
    suite_capacity_overflow();
    suite_end_gates();
    suite_legacy_false_alloc();
    suite_untracked();
    printf("\n%s: %d total, %d passed, %d failed\n",
           "test_aarch64_sched_observer",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
