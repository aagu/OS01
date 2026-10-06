/*
 * hosttests/mock/fb_resolution_runtime.c
 *
 * Mock runtime implementation for fb_state and fb_map host testing.
 */
#include "fb_resolution_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <stdio.h>

struct task_struct mock_current_task = {
    .wakeup_ns = 0,
    .pid = 1,
};

uint64_t g_mock_clock_ns = 1000000000ULL;
uint64_t g_mock_clock_step_ns = 0;
bool     g_mock_use_real_clock = false;
bool     g_mock_yield_on_wait = false;
uint32_t g_mock_tlb_shootdown_count = 0;
uint32_t g_mock_flush_tlb_count = 0;
int      g_mock_alloc_table_fail_countdown = -1;
bool     g_mock_corrupt_pmd_phys = false;
uint32_t g_mock_hardware_call_count = 0;

/* ── Mutex mock ── */
void mutex_init(mutex_t *m)
{
    m->owner = 0;
}

void mutex_lock(mutex_t *m)
{
    while (!__sync_bool_compare_and_swap(&m->owner, 0, 1)) {
        sched_yield();
    }
}

void mutex_unlock(mutex_t *m)
{
    __sync_lock_release(&m->owner);
}

int mutex_trylock(mutex_t *m)
{
    return __sync_bool_compare_and_swap(&m->owner, 0, 1) ? 1 : 0;
}

/* ── Clocksource mock ── */
uint64_t arch_clocksource_read_ns(void)
{
    if (g_mock_use_real_clock) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
    return g_mock_clock_ns;
}

/* ── Blocker wait mock ── */
int blocker_wait(blocker_check_t check, int type, bool signal_can_wake)
{
    (void)type;
    (void)signal_can_wake;

    if (!g_mock_use_real_clock && g_mock_clock_step_ns > 0) {
        g_mock_clock_ns += g_mock_clock_step_ns;
    }

    if (g_mock_yield_on_wait) {
        sched_yield();
        usleep(200); /* 0.2 ms */
    }

    if (check) {
        (void)check(current);
    }
    return 0;
}

/* ── Page table mock for fb_map ── */
#define MOCK_PAGE_POOL_SIZE 64
static void *g_page_pool[MOCK_PAGE_POOL_SIZE];
static size_t g_page_pool_count = 0;

static uint64_t g_root_pgd[512] __attribute__((aligned(4096)));
uint64_t *kernel_map = g_root_pgd;

static void *alloc_mock_page_table(void)
{
    if (g_page_pool_count >= MOCK_PAGE_POOL_SIZE) {
        return NULL;
    }
    void *ptr = NULL;
    if (posix_memalign(&ptr, 4096, 4096) != 0) {
        return NULL;
    }
    memset(ptr, 0, 4096);
    g_page_pool[g_page_pool_count++] = ptr;
    return ptr;
}

int vmm_get_next_level_checked(uint64_t *current_level, size_t entry,
                               uint64_t flags, uint64_t *out_pa)
{
    if (!current_level || entry >= 512) {
        return -EINVAL;
    }

    if (!(current_level[entry] & 1)) {
        if (g_mock_alloc_table_fail_countdown == 0) {
            return -ENOMEM;
        }
        if (g_mock_alloc_table_fail_countdown > 0) {
            g_mock_alloc_table_fail_countdown--;
        }

        void *table = alloc_mock_page_table();
        if (!table) {
            return -ENOMEM;
        }
        uint64_t pa = (uint64_t)(uintptr_t)table;
        current_level[entry] = pa | flags;
    }

    if (out_pa) {
        *out_pa = current_level[entry] & ~0xFFFULL;
    }
    return 0;
}

void tlb_shootdown(void)
{
    g_mock_tlb_shootdown_count++;
}

void flush_tlb(void)
{
    g_mock_flush_tlb_count++;
}

void fb_resolution_runtime_reset(void)
{
    mock_current_task.wakeup_ns = 0;
    mock_current_task.pid = 1;

    g_mock_clock_ns = 1000000000ULL;
    g_mock_clock_step_ns = 0;
    g_mock_use_real_clock = false;
    g_mock_yield_on_wait = false;
    g_mock_tlb_shootdown_count = 0;
    g_mock_flush_tlb_count = 0;
    g_mock_alloc_table_fail_countdown = -1;
    g_mock_corrupt_pmd_phys = false;
    g_mock_hardware_call_count = 0;

    memset(g_root_pgd, 0, sizeof(g_root_pgd));
    for (size_t i = 0; i < g_page_pool_count; i++) {
        free(g_page_pool[i]);
        g_page_pool[i] = NULL;
    }
    g_page_pool_count = 0;
}
