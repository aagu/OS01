/* Concurrency-safe resource observer for the aarch64 EL1 scheduler
 * selftests (P1 Task 2, spec §6).
 *
 * Guard: the x86 kernel build collects every selftest C file and the
 * production aarch64 build compiles this file without OS01_SELFTEST —
 * in both shapes the TU must be empty.  The host observer test
 * compiles the REAL module with OS01_HOST_SCHED_OBSERVER.
 */
#if defined(OS01_SELFTEST) && \
    (defined(__aarch64__) || defined(OS01_HOST_SCHED_OBSERVER))

#include <selftest/aarch64_sched_observer.h>
#include <arch/spinlock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Record states.  FREE slots are reusable; FREED slots are reusable
 * too — only ALLOCATED/FREEING records occupy live capacity. */
enum {
    OBS_SLOT_FREE = 0,
    OBS_ALLOCATED = 1,
    OBS_FREEING = 2,
    OBS_FREED = 3,
};

typedef struct {
    const void *ptr;
    uint64_t token;
    uint32_t kind;
    uint32_t generation;
    uint32_t state;
} obs_record_t;

/* The dedicated IRQ-safe lock: the ONLY lock this module touches, and
 * nothing is allocated, printed or rq-locked inside it.  Explicitly
 * initialised UNLOCKED (lock==1): a zero-initialised spinlock is
 * permanently locked and the first observer call would spin forever. */
static spinlock_T obs_lock = { .lock = 1 };
static obs_record_t obs_records[AARCH64_SCHED_OBSERVER_CAPACITY];
static uint32_t obs_active_cpu;
static uint32_t obs_active_generation;   /* 0 = no batch open */
static uint64_t obs_next_token = 1;      /* monotonic, never reused */
static uint64_t obs_created;
static uint64_t obs_released[4];         /* per kind (1..3) */

/* Batch-scoped failure flags (cleared by begin, inspected by end). */
static uint32_t obs_overflow;
static uint32_t obs_bad_false_alloc;
static uint32_t obs_unknown_token;
static uint32_t obs_duplicate_complete;
static uint32_t obs_begin_while_active;

static int obs_scan_failures(void);

void aarch64_sched_observer_begin(unsigned cpu, unsigned generation)
{
    uint64_t flags = spin_lock_irqsave(&obs_lock);
    /* Only one batch may be active.  A begin over a batch that already
     * FAILED its end supersedes it silently (that batch is terminal);
     * a begin over a clean, still-open batch is a double-begin bug and
     * fails the NEW batch. */
    if (obs_active_generation != 0 && !obs_scan_failures())
        obs_begin_while_active = 1;
    obs_active_cpu = cpu;
    obs_active_generation = generation;
    obs_overflow = 0;
    obs_bad_false_alloc = 0;
    obs_unknown_token = 0;
    obs_duplicate_complete = 0;
    spin_unlock_irqrestore(&obs_lock, flags);
}

static int obs_scan_failures(void)
{
    return obs_overflow || obs_bad_false_alloc || obs_unknown_token ||
           obs_duplicate_complete || obs_begin_while_active;
}

int aarch64_sched_observer_end(unsigned cpu, unsigned generation)
{
    int failed = 0;
    uint64_t flags = spin_lock_irqsave(&obs_lock);
    if (obs_active_generation == 0 ||
        obs_active_cpu != cpu || obs_active_generation != generation) {
        failed = 1;
    } else {
        for (unsigned i = 0; i < AARCH64_SCHED_OBSERVER_CAPACITY; ++i) {
            obs_record_t *r = &obs_records[i];
            if (r->generation != obs_active_generation)
                continue;
            if (r->state == OBS_ALLOCATED || r->state == OBS_FREEING) {
                failed = 1;
                break;
            }
        }
        if (!failed && obs_scan_failures())
            failed = 1;
        if (!failed)
            obs_active_generation = 0;   /* deactivate only when clean */
    }
    spin_unlock_irqrestore(&obs_lock, flags);
    return failed;
}

void aarch64_sched_observe_resource(const void *ptr, unsigned kind,
                                    bool alloc)
{
    if (!ptr || kind < AARCH64_SCHED_RESOURCE_STACK ||
        kind > AARCH64_SCHED_RESOURCE_FPU)
        return;
    uint64_t flags = spin_lock_irqsave(&obs_lock);
    if (obs_active_generation == 0) {
        spin_unlock_irqrestore(&obs_lock, flags);
        return;
    }
    if (!alloc) {
        /* A false call used to fake a release completion; it is now a
         * recorded batch failure so a leftover caller cannot hide. */
        obs_bad_false_alloc = 1;
        spin_unlock_irqrestore(&obs_lock, flags);
        return;
    }
    obs_record_t *slot = NULL;
    for (unsigned i = 0; i < AARCH64_SCHED_OBSERVER_CAPACITY; ++i) {
        if (obs_records[i].state == OBS_SLOT_FREE ||
            obs_records[i].state == OBS_FREED) {
            slot = &obs_records[i];
            break;
        }
    }
    if (!slot) {
        obs_overflow = 1;    /* full table: the batch cannot end clean */
        spin_unlock_irqrestore(&obs_lock, flags);
        return;
    }
    slot->ptr = ptr;
    slot->kind = kind;
    slot->generation = obs_active_generation;
    slot->token = obs_next_token++;
    slot->state = OBS_ALLOCATED;
    obs_created++;
    spin_unlock_irqrestore(&obs_lock, flags);
}

uint64_t aarch64_sched_observe_free_begin(const void *ptr, unsigned kind)
{
    uint64_t token = 0;
    uint64_t flags = spin_lock_irqsave(&obs_lock);
    if (obs_active_generation != 0 && ptr) {
        /* LATEST matching ALLOCATED record wins (highest token), so a
         * same-address re-allocation while an older record is still
         * FREEING is never confused with it. */
        obs_record_t *match = NULL;
        for (unsigned i = 0; i < AARCH64_SCHED_OBSERVER_CAPACITY; ++i) {
            obs_record_t *r = &obs_records[i];
            if (r->state == OBS_ALLOCATED && r->ptr == ptr &&
                r->kind == kind &&
                r->generation == obs_active_generation &&
                (!match || r->token > match->token))
                match = r;
        }
        if (match) {
            match->state = OBS_FREEING;
            token = match->token;
        }
    }
    spin_unlock_irqrestore(&obs_lock, flags);
    return token;
}

void aarch64_sched_observe_free_complete(uint64_t token)
{
    if (token == 0)
        return;   /* untracked pointer: nothing to record */
    uint64_t flags = spin_lock_irqsave(&obs_lock);
    obs_record_t *match = NULL;
    for (unsigned i = 0; i < AARCH64_SCHED_OBSERVER_CAPACITY; ++i) {
        if (obs_records[i].token == token) {
            match = &obs_records[i];
            break;
        }
    }
    if (!match) {
        obs_unknown_token = 1;
    } else if (match->state == OBS_FREEING) {
        match->state = OBS_FREED;
        obs_released[match->kind]++;
    } else {
        /* ALLOCATED (no free_begin) or FREED (already completed). */
        obs_duplicate_complete = 1;
    }
    spin_unlock_irqrestore(&obs_lock, flags);
}

uint64_t aarch64_sched_observer_created(void)
{
    uint64_t flags = spin_lock_irqsave(&obs_lock);
    uint64_t v = obs_created;
    spin_unlock_irqrestore(&obs_lock, flags);
    return v;
}

uint64_t aarch64_sched_observer_released(unsigned kind)
{
    uint64_t flags = spin_lock_irqsave(&obs_lock);
    uint64_t v = kind < 4 ? obs_released[kind] : 0;
    spin_unlock_irqrestore(&obs_lock, flags);
    return v;
}

#endif /* OS01_SELFTEST && (aarch64 || host observer) */
