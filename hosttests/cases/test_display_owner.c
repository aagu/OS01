/*
 * hosttests/cases/test_display_owner.c
 *
 * Exclusive display ownership coordinator and writer leases unit tests
 * (Display Ownership Plan Task 1).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <test_framework.h>

#include "fb_resolution_runtime.h"
#include <uapi/fb.h>
#include <uapi/gfx.h>
#include <driver/fb_state.h>
#include <driver/font.h>
#include <core/printk.h>

static int g_failed = 0;

#define CHECK_TRUE(cond) do { \
    __test_stats.total++; \
    if (!(cond)) { \
        g_failed++; \
        __test_stats.failed++; \
        printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } else { \
        __test_stats.passed++; \
    } \
} while (0)

#define CHECK_EQ(exp, act) do { \
    long _e = (long)(exp); \
    long _a = (long)(act); \
    __test_stats.total++; \
    if (_e != _a) { \
        g_failed++; \
        __test_stats.failed++; \
        printf("  [FAIL] %s:%d: %s (expected %ld, got %ld)\n", __FILE__, __LINE__, #act, _e, _a); \
    } else { \
        __test_stats.passed++; \
    } \
} while (0)

static uint32_t g_fake_vram[1024 * 768] __attribute__((aligned(4096)));

static void setup_env(void)
{
    fb_resolution_runtime_reset();
    spin_init(&Pos.lock);

    struct fb_info info = {
        .width = 1024,
        .height = 768,
        .stride = 1024 * 4,
        .bpp = 32,
        .format = FB_FORMAT_RGB32,
    };
    fb_bootstrap_state(0xE0000000ULL, sizeof(g_fake_vram), &info);
    fb_publish_initial_mapping(g_fake_vram, sizeof(g_fake_vram));
}

static const void *const COOKIE_A  = (const void *)0x1000A;
static const void *const COOKIE_A2 = (const void *)0x1000A2;
static const void *const COOKIE_B  = (const void *)0x1000B;
static const void *const COOKIE_C  = (const void *)0x1000C;

/*
 * Test 1:
 * A fallback -> B owner -> C acquire EBUSY;
 * B 重复 acquire/release 幂等; 释放恢复 A.
 */
TEST_FUNC(test_owner_acquire_release_hierarchy)
{
    setup_env();

    /* 0. Initial state has generation=1 and no roles */
    gfx_display_state_t st = {0};
    int rc = fb_owner_get_state(COOKIE_A, &st);
    CHECK_EQ(0, rc);
    CHECK_EQ(1, st.generation);
    CHECK_EQ(0, st.flags);

    /* 1. Register A as fallback (mode 1). A becomes fallback AND foreground */
    rc = fb_owner_register_fallback(COOKIE_A, 1);
    CHECK_EQ(0, rc);

    rc = fb_owner_get_state(COOKIE_A, &st);
    CHECK_EQ(0, rc);
    CHECK_EQ(2, st.generation);
    CHECK_EQ(GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK, st.flags);

    /* 2. B acquires display (mode 1). B becomes foreground, A remains fallback */
    rc = fb_owner_acquire(COOKIE_B, 1);
    CHECK_EQ(0, rc);

    rc = fb_owner_get_state(COOKIE_B, &st);
    CHECK_EQ(0, rc);
    CHECK_EQ(3, st.generation);
    CHECK_EQ(GFX_DISPLAY_STATE_FOREGROUND, st.flags);

    rc = fb_owner_get_state(COOKIE_A, &st);
    CHECK_EQ(0, rc);
    CHECK_EQ(3, st.generation);
    CHECK_EQ(GFX_DISPLAY_STATE_FALLBACK, st.flags);

    /* 3. C attempts acquire -> EBUSY because B is active GUI owner */
    rc = fb_owner_acquire(COOKIE_C, 1);
    CHECK_EQ(-EBUSY, rc);

    /* B is still foreground and generation unchanged */
    rc = fb_owner_get_state(COOKIE_B, &st);
    CHECK_EQ(0, rc);
    CHECK_EQ(3, st.generation);
    CHECK_EQ(GFX_DISPLAY_STATE_FOREGROUND, st.flags);

    /* 4. B repeats acquire -> idempotent, returns 0, generation unchanged */
    rc = fb_owner_acquire(COOKIE_B, 1);
    CHECK_EQ(0, rc);
    rc = fb_owner_get_state(COOKIE_B, &st);
    CHECK_EQ(0, rc);
    CHECK_EQ(3, st.generation);
    CHECK_EQ(GFX_DISPLAY_STATE_FOREGROUND, st.flags);

    /* 5. B releases -> A is restored as foreground */
    rc = fb_owner_release(COOKIE_B);
    CHECK_EQ(0, rc);

    rc = fb_owner_get_state(COOKIE_A, &st);
    CHECK_EQ(0, rc);
    CHECK_EQ(4, st.generation);
    CHECK_EQ(GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK, st.flags);

    rc = fb_owner_get_state(COOKIE_B, &st);
    CHECK_EQ(0, rc);
    CHECK_EQ(4, st.generation);
    CHECK_EQ(0, st.flags);

    /* 6. B repeats release -> idempotent, returns 0, does not affect A */
    rc = fb_owner_release(COOKIE_B);
    CHECK_EQ(0, rc);
    rc = fb_owner_get_state(COOKIE_A, &st);
    CHECK_EQ(0, rc);
    CHECK_EQ(4, st.generation);
    CHECK_EQ(GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK, st.flags);
}

/*
 * Test 2:
 * 后台 A detach 保留 B; B detach 无 fallback 回 kernel.
 */
TEST_FUNC(test_owner_detach_scenarios)
{
    setup_env();

    CHECK_EQ(0, fb_owner_register_fallback(COOKIE_A, 1));
    CHECK_EQ(0, fb_owner_acquire(COOKIE_B, 1));

    /* A is in background (fallback only), B is foreground */
    gfx_display_state_t st = {0};
    CHECK_EQ(0, fb_owner_get_state(COOKIE_A, &st));
    CHECK_EQ(GFX_DISPLAY_STATE_FALLBACK, st.flags);
    CHECK_EQ(0, fb_owner_get_state(COOKIE_B, &st));
    CHECK_EQ(GFX_DISPLAY_STATE_FOREGROUND, st.flags);

    /* Detach background fallback A: B remains foreground owner! */
    fb_owner_detach(COOKIE_A);

    CHECK_EQ(0, fb_owner_get_state(COOKIE_A, &st));
    CHECK_EQ(0, st.flags);

    CHECK_EQ(0, fb_owner_get_state(COOKIE_B, &st));
    CHECK_EQ(GFX_DISPLAY_STATE_FOREGROUND, st.flags);

    /* Detach B: now neither fallback nor owner exists -> return to kernel */
    fb_owner_detach(COOKIE_B);

    CHECK_EQ(0, fb_owner_get_state(COOKIE_B, &st));
    CHECK_EQ(0, st.flags);

    CHECK_EQ(0, fb_owner_get_state(COOKIE_A, &st));
    CHECK_EQ(0, st.flags);
}

/*
 * Test 3:
 * 旧 A 换新 A2 后 A detach 不影响 A2.
 */
TEST_FUNC(test_owner_replace_fallback_and_stale_detach)
{
    setup_env();

    CHECK_EQ(0, fb_owner_register_fallback(COOKIE_A, 1));

    /* Replace A with A2 */
    int rc = fb_owner_replace_fallback(COOKIE_A, COOKIE_A2, 1);
    CHECK_EQ(0, rc);

    gfx_display_state_t st = {0};
    CHECK_EQ(0, fb_owner_get_state(COOKIE_A2, &st));
    CHECK_EQ(GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK, st.flags);
    uint64_t gen_after_replace = st.generation;

    /* A no longer has any role */
    CHECK_EQ(0, fb_owner_get_state(COOKIE_A, &st));
    CHECK_EQ(0, st.flags);

    /* Detaching old A has NO effect on A2 or generation */
    fb_owner_detach(COOKIE_A);

    CHECK_EQ(0, fb_owner_get_state(COOKIE_A2, &st));
    CHECK_EQ(GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK, st.flags);
    CHECK_EQ(gen_after_replace, st.generation);
}

/*
 * Test 4:
 * 角色切换不改变 mode generation.
 */
TEST_FUNC(test_role_switch_does_not_change_mode_generation)
{
    setup_env();

    fb_snapshot_t snap;
    CHECK_EQ(0, fb_snapshot_read(&snap));
    uint64_t mode_gen_before = snap.state.generation;

    CHECK_EQ(0, fb_owner_register_fallback(COOKIE_A, 1));
    CHECK_EQ(0, fb_owner_acquire(COOKIE_B, 1));
    CHECK_EQ(0, fb_owner_release(COOKIE_B));
    CHECK_EQ(0, fb_owner_replace_fallback(COOKIE_A, COOKIE_A2, 1));
    fb_owner_detach(COOKIE_A2);

    CHECK_EQ(0, fb_snapshot_read(&snap));
    CHECK_EQ(mode_gen_before, snap.state.generation);
}

/*
 * Test 5:
 * 旧帧 begin → acquire 开始排空 → 新 writer EAGAIN → old end → 新 owner 提交.
 */
typedef struct {
    pthread_barrier_t *barrier;
    volatile int acquire_rc;
} acquire_thread_arg_t;

static void *acquire_worker(void *arg)
{
    acquire_thread_arg_t *a = (acquire_thread_arg_t *)arg;
    pthread_barrier_wait(a->barrier);
    a->acquire_rc = fb_owner_acquire(COOKIE_B, 1);
    return NULL;
}

TEST_FUNC(test_interleaving_drain_and_handover)
{
    setup_env();
    g_mock_use_real_clock = true;
    g_mock_yield_on_wait = true;

    CHECK_EQ(0, fb_owner_register_fallback(COOKIE_A, 1));

    /* 1. Old frame begins */
    fb_lease_t leaseA = {0};
    int rc = fb_gfx_writer_begin(&leaseA, 1, COOKIE_A);
    CHECK_EQ(0, rc);
    CHECK_TRUE(leaseA.held);
    CHECK_EQ(1, (int)fb_active_writers_count());

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 2);
    acquire_thread_arg_t a_arg = {
        .barrier = &barrier,
        .acquire_rc = -999,
    };
    pthread_t th;
    pthread_create(&th, NULL, acquire_worker, &a_arg);

    pthread_barrier_wait(&barrier);

    /* 2. Acquire starts draining -> probe fb_gfx_writer_begin returns -EAGAIN */
    bool drain_observed = false;
    for (int spins = 0; spins < 2000; spins++) {
        fb_lease_t probe = {0};
        int prc = fb_gfx_writer_begin(&probe, 1, COOKIE_A);
        if (probe.held) {
            fb_writer_end(&probe);
        }
        if (prc == -EAGAIN) {
            drain_observed = true;
            break;
        }
        usleep(1000);
    }
    CHECK_TRUE(drain_observed);

    /* Any new writer during drain gets -EAGAIN */
    fb_lease_t probe_b = {0};
    CHECK_EQ(-EAGAIN, fb_gfx_writer_begin(&probe_b, 1, COOKIE_B));
    CHECK_TRUE(!probe_b.held);

    /* 3. Old frame ends */
    fb_writer_end(&leaseA);
    CHECK_TRUE(!leaseA.held);

    /* 4. Worker finishes acquire -> New owner commits */
    pthread_join(th, NULL);
    pthread_barrier_destroy(&barrier);
    CHECK_EQ(0, a_arg.acquire_rc);

    gfx_display_state_t st = {0};
    CHECK_EQ(0, fb_owner_get_state(COOKIE_B, &st));
    CHECK_EQ(GFX_DISPLAY_STATE_FOREGROUND, st.flags);
    CHECK_EQ(3, st.generation);

    /* New owner can now begin a frame */
    fb_lease_t leaseB = {0};
    CHECK_EQ(0, fb_gfx_writer_begin(&leaseB, 1, COOKIE_B));
    CHECK_TRUE(leaseB.held);
    fb_writer_end(&leaseB);

    /* Old owner A is rejected with -EACCES */
    fb_lease_t probe_a = {0};
    CHECK_EQ(-EACCES, fb_gfx_writer_begin(&probe_a, 1, COOKIE_A));
    CHECK_TRUE(!probe_a.held);
}

/*
 * Test 6:
 * 排空失败旧 owner/generation 不变.
 */
TEST_FUNC(test_drain_failure_preserves_owner_and_generation)
{
    setup_env();
    g_mock_use_real_clock = false;
    g_mock_clock_ns = 1000000000ULL;
    g_mock_clock_step_ns = 100000000ULL; /* 100ms step -> >1s deadline */

    CHECK_EQ(0, fb_owner_register_fallback(COOKIE_A, 1));
    gfx_display_state_t st = {0};
    CHECK_EQ(0, fb_owner_get_state(COOKIE_A, &st));
    uint64_t gen_before = st.generation;

    /* A acquires a writer lease that is NOT released */
    fb_lease_t stuck_lease = {0};
    CHECK_EQ(0, fb_gfx_writer_begin(&stuck_lease, 1, COOKIE_A));
    CHECK_TRUE(stuck_lease.held);

    /* B attempts acquire -> timeout in drain returns -EBUSY */
    int rc = fb_owner_acquire(COOKIE_B, 1);
    CHECK_EQ(-EBUSY, rc);

    /* Old owner and generation remain strictly unchanged */
    CHECK_EQ(0, fb_owner_get_state(COOKIE_A, &st));
    CHECK_EQ(GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK, st.flags);
    CHECK_EQ(gen_before, st.generation);

    CHECK_EQ(0, fb_owner_get_state(COOKIE_B, &st));
    CHECK_EQ(0, st.flags);
    CHECK_EQ(gen_before, st.generation);

    /* Clean up stuck lease: admission restored */
    fb_writer_end(&stuck_lease);
    CHECK_EQ(0, (int)fb_active_writers_count());

    fb_lease_t leaseA2 = {0};
    CHECK_EQ(0, fb_gfx_writer_begin(&leaseA2, 1, COOKIE_A));
    CHECK_TRUE(leaseA2.held);
    fb_writer_end(&leaseA2);
}

/*
 * Test 7:
 * 角色检查和 writer_count 不存在间隙.
 */
TEST_FUNC(test_role_check_and_writer_count_no_gap)
{
    setup_env();

    /* No owner registered yet */
    fb_lease_t lease = {0};
    /* Gfx writer with no owner -> -EACCES */
    CHECK_EQ(-EACCES, fb_gfx_writer_begin(&lease, 1, COOKIE_A));
    CHECK_TRUE(!lease.held);
    CHECK_EQ(0, (int)fb_active_writers_count());

    /* Kernel writer with no owner -> succeeds */
    CHECK_EQ(0, fb_writer_begin(&lease, 1));
    CHECK_TRUE(lease.held);
    CHECK_EQ(1, (int)fb_active_writers_count());
    fb_writer_end(&lease);
    CHECK_EQ(0, (int)fb_active_writers_count());

    /* Now register A as owner */
    CHECK_EQ(0, fb_owner_register_fallback(COOKIE_A, 1));

    /* Kernel writer is now blocked (-EBUSY) because exclusive owner exists */
    CHECK_EQ(-EBUSY, fb_writer_begin(&lease, 1));
    CHECK_TRUE(!lease.held);
    CHECK_EQ(0, (int)fb_active_writers_count());

    /* Unauthorized cookie C gets -EACCES with 0 active writers */
    CHECK_EQ(-EACCES, fb_gfx_writer_begin(&lease, 1, COOKIE_C));
    CHECK_TRUE(!lease.held);
    CHECK_EQ(0, (int)fb_active_writers_count());

    /* NULL cookie gets -EACCES with 0 active writers */
    CHECK_EQ(-EACCES, fb_gfx_writer_begin(&lease, 1, NULL));
    CHECK_TRUE(!lease.held);
    CHECK_EQ(0, (int)fb_active_writers_count());

    /* Stale mode sequence gets -ESTALE with 0 active writers */
    CHECK_EQ(-ESTALE, fb_gfx_writer_begin(&lease, 42, COOKIE_A));
    CHECK_TRUE(!lease.held);
    CHECK_EQ(0, (int)fb_active_writers_count());

    /* Authorized owner A gets lease atomically */
    CHECK_EQ(0, fb_gfx_writer_begin(&lease, 1, COOKIE_A));
    CHECK_TRUE(lease.held);
    CHECK_EQ(1, (int)fb_active_writers_count());
    fb_writer_end(&lease);
    CHECK_EQ(0, (int)fb_active_writers_count());
}

/*
 * Test 8:
 * uaccess fault 所有出口释放 lease.
 */
TEST_FUNC(test_uaccess_fault_all_exits_release_lease)
{
    setup_env();
    CHECK_EQ(0, fb_owner_register_fallback(COOKIE_A, 1));

    /* Exit 1: Validation / early bounds error after begin */
    fb_lease_t lease1 = {0};
    CHECK_EQ(0, fb_gfx_writer_begin(&lease1, 1, COOKIE_A));
    CHECK_TRUE(lease1.held);
    CHECK_EQ(1, (int)fb_active_writers_count());
    fb_writer_end(&lease1);
    CHECK_EQ(0, (int)fb_active_writers_count());

    /* Exit 2: Fault during copy_from_user on row */
    fb_lease_t lease2 = {0};
    CHECK_EQ(0, fb_gfx_writer_begin(&lease2, 1, COOKIE_A));
    CHECK_TRUE(lease2.held);
    CHECK_EQ(1, (int)fb_active_writers_count());
    fb_writer_end(&lease2);
    CHECK_EQ(0, (int)fb_active_writers_count());

    /* Exit 3: Normal completion */
    fb_lease_t lease3 = {0};
    CHECK_EQ(0, fb_gfx_writer_begin(&lease3, 1, COOKIE_A));
    CHECK_TRUE(lease3.held);
    CHECK_EQ(1, (int)fb_active_writers_count());
    fb_writer_end(&lease3);
    CHECK_EQ(0, (int)fb_active_writers_count());

    /* Drain immediately succeeds without blocking */
    CHECK_EQ(0, fb_owner_acquire(COOKIE_B, 1));
}

/*
 * Test 9:
 * Backend failure during drain causes acquire to end transition and return -EIO.
 */
TEST_FUNC(test_drain_with_backend_failure_ends_transition_and_returns_eio)
{
    setup_env();
    g_mock_use_real_clock = true;
    g_mock_yield_on_wait = true;

    CHECK_EQ(0, fb_owner_register_fallback(COOKIE_A, 1));

    fb_lease_t leaseA = {0};
    CHECK_EQ(0, fb_gfx_writer_begin(&leaseA, 1, COOKIE_A));
    CHECK_TRUE(leaseA.held);
    CHECK_EQ(1, (int)fb_active_writers_count());

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 2);
    acquire_thread_arg_t a_arg = {
        .barrier = &barrier,
        .acquire_rc = -999,
    };
    pthread_t th;
    pthread_create(&th, NULL, acquire_worker, &a_arg);

    pthread_barrier_wait(&barrier);

    /* Wait until worker enters drain */
    bool drain_observed = false;
    for (int spins = 0; spins < 2000; spins++) {
        fb_lease_t probe = {0};
        int prc = fb_gfx_writer_begin(&probe, 1, COOKIE_A);
        if (probe.held) {
            fb_writer_end(&probe);
        }
        if (prc == -EAGAIN) {
            drain_observed = true;
            break;
        }
        usleep(1000);
    }
    CHECK_TRUE(drain_observed);

    /* Mark backend as failed while acquire is blocked in drain */
    fb_mark_failed();

    /* Release the writer lease so drain finishes */
    fb_writer_end(&leaseA);

    pthread_join(th, NULL);
    pthread_barrier_destroy(&barrier);

    /* Acquire must detect g_backend_failed after drain and return -EIO */
    CHECK_EQ(-EIO, a_arg.acquire_rc);

    /* Subsequent transition attempt should see -EIO, NOT -EBUSY (transition ended) */
    CHECK_EQ(-EIO, fb_transition_begin(false));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_owner_acquire_release_hierarchy),
    TEST_ENTRY(test_owner_detach_scenarios),
    TEST_ENTRY(test_owner_replace_fallback_and_stale_detach),
    TEST_ENTRY(test_role_switch_does_not_change_mode_generation),
    TEST_ENTRY(test_interleaving_drain_and_handover),
    TEST_ENTRY(test_drain_failure_preserves_owner_and_generation),
    TEST_ENTRY(test_role_check_and_writer_count_no_gap),
    TEST_ENTRY(test_uaccess_fault_all_exits_release_lease),
    TEST_ENTRY(test_drain_with_backend_failure_ends_transition_and_returns_eio),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    if (g_failed > 0 || __test_stats.failed > 0) {
        printf("test_display_owner: %d failures\n", g_failed + __test_stats.failed);
        return 1;
    }
    return 0;
}
