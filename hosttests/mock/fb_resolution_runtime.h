/*
 * hosttests/mock/fb_resolution_runtime.h
 *
 * Mock runtime environment for fb_state and fb_map host testing.
 */
#ifndef OS01_FB_RESOLUTION_RUNTIME_H
#define OS01_FB_RESOLUTION_RUNTIME_H

#define OS01_HOST_SPINLOCK_T_PROVIDED 1
#define __SPINLOCK_H__ 1
#define _ARCH_SPINLOCK_H 1
#define _KERNEL_MUTEX_H 1
#define _KERNEL_WAIT_H 1
#define _ARCH_CLOCKSOURCE_H 1
#define KERNEL_TASK_H 1
#define _ARCH_MMU_H 1

typedef struct { int dummy; } wait_queue_t;

#define ARCH_PAGE_OFFSET 0ULL
#define PAGE_OFFSET 0ULL
#ifndef Phy_To_Virt
#define Phy_To_Virt(x) ((void *)(uintptr_t)(x))
#endif
#ifndef Virt_To_Phy
#define Virt_To_Phy(x) ((uintptr_t)(x))
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Thread-safe spinlock for host tests ── */
typedef struct {
    volatile int lock;
} spinlock_T;

static inline void spin_init(spinlock_T *l) {
    l->lock = 0;
}

static inline void spin_lock(spinlock_T *l) {
    while (__atomic_test_and_set(&l->lock, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
}

static inline void spin_unlock(spinlock_T *l) {
    __atomic_clear(&l->lock, __ATOMIC_RELEASE);
}

static inline long spin_trylock(spinlock_T *l) {
    int expected = 0;
    return __atomic_compare_exchange_n(&l->lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 1 : 0;
}

static inline uint64_t spin_lock_irqsave(spinlock_T *l) {
    spin_lock(l);
    return 0;
}

static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f) {
    (void)f;
    spin_unlock(l);
}

/* ── Mutex mock ── */
typedef struct {
    volatile int64_t owner;
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);
int  mutex_trylock(mutex_t *m);

/* ── Task and blocker_wait mock ── */
#define BLOCKER_NANOSLEEP 2

struct mm_struct {
    uint64_t *pgdir;
};

struct task_struct {
    uint64_t wakeup_ns;
    int64_t pid;
    struct mm_struct *mm;
};

extern struct task_struct mock_current_task;
#define current (&mock_current_task)

#ifndef arch_flush_tlb_all
#define arch_flush_tlb_all() flush_tlb()
#endif

typedef bool (*blocker_check_t)(struct task_struct *waiter);
int blocker_wait(blocker_check_t check, int type, bool signal_can_wake);

uint64_t arch_clocksource_read_ns(void);

/* ── Mock page table / MMU surface for fb_map ── */
extern uint64_t *kernel_map;
int vmm_get_next_level_checked(uint64_t *current_level, size_t entry,
                               uint64_t flags, uint64_t *out_pa);
void tlb_shootdown(void);
void flush_tlb(void);

/* ── Mock control knobs for tests ── */
extern uint64_t g_mock_clock_ns;
extern uint64_t g_mock_clock_step_ns;
extern bool     g_mock_use_real_clock;
extern bool     g_mock_yield_on_wait;
extern uint32_t g_mock_tlb_shootdown_count;
extern uint32_t g_mock_flush_tlb_count;
extern int      g_mock_alloc_table_fail_countdown;
extern bool     g_mock_corrupt_pmd_phys;
extern uint32_t g_mock_hardware_call_count;

void fb_resolution_runtime_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OS01_FB_RESOLUTION_RUNTIME_H */
