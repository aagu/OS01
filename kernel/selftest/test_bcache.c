// kernel/selftest/test_bcache.c — ARCH-6 block cache real-SMP selftest.
//
// Runs from task_init's BSP context (sched_boot.c) after scheduler_ok=1,
// reported as the late case "bcache_smp_coherence".  Two pinned kthreads
// (harness CPU + one peer) drive the production cache through the same
// paths the fs drivers use:
//
//   same_sector_atomic        writer/reader on one key: every read must
//                             observe one complete write (A or B)
//   same_sector_concurrent_miss  stalled fill + second reader on the same
//                             key: exactly one backend read, both agree
//   adjacent_sector_isolation two writers on neighbouring keys
//   flush_waits_for_reader    flusher drains behind a held pin (ARM→wake)
//   exhaustion_waits          all slots pinned; new key waits, then wins
//                             a slot when the script releases one pin
//   flush_backend_visibility  flush + direct backend read
//   no_leaks                  pinned/busy/waiters/dirty back to zero,
//                             silent unregister drops all test entries
//
// The harness (idle-task context) only spawns workers, flips flags,
// samples stats and yields — every operation that can sleep on the cache
// runs in a pid>0 pinned worker.  Evicting real boot-disk cache entries
// while the fake disk fills the pool is expected and allowed; the global
// reset hook is NEVER called here.
#if defined(OS01_SELFTEST) && !defined(__aarch64__)

#include <core/printk.h>
#include <sched/task.h>
#include <block/bcache.h>
#include <block/blockdev.h>
#include <percpu/percpu.h>
#include <arch/irq.h>
#include <time/timer.h>
#include <string.h>
#include <errno.h>
#include <kernel.h>

#define BC_WAIT_TICKS 500                       // 5 s at the 100 Hz tick
#define BC_SECTORS    (BCACHE_BUFS + 40)
#define BC_KEY_STALL  30
#define BC_KEY_FLUSH  20
#define BC_KEY_EXH    BCACHE_BUFS               // first beyond-the-pool key

static uint8_t bc_backend[BC_SECTORS][512];
static block_device_t *bc_dev;

// fake-device control (all atomically accessed)
static int bc_reads;                    // backend read calls
static volatile int bc_stall_key;       // -1 = no stall
static volatile int bc_stall_entered;
static volatile int bc_stall_release;
static volatile int bc_stall_consumed;

static int worker_wait(const volatile int *flag);

static int fake_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev;
    __atomic_fetch_add(&bc_reads, 1, __ATOMIC_RELAXED);
    if ((int)lba == bc_stall_key && !bc_stall_consumed) {
        __atomic_store_n(&bc_stall_entered, 1, __ATOMIC_RELEASE);
        if (!worker_wait(&bc_stall_release))
            return -ETIMEDOUT;
        bc_stall_consumed = 1;
    }
    for (uint32_t i = 0; i < count; i++)
        if (lba + i < BC_SECTORS)
            memcpy((uint8_t *)buf + i * 512, bc_backend[lba + i], 512);
    return 0;
}

static int fake_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    for (uint32_t i = 0; i < count; i++)
        if (lba + i < BC_SECTORS)
            memcpy(bc_backend[lba + i], (const uint8_t *)buf + i * 512, 512);
    return 0;
}

static int fake_flush(block_device_t *dev)
{
    (void)dev;
    return 0;
}

static const struct block_device_ops bc_ops = {
    .read = fake_read, .write = fake_write, .flush = fake_flush,
};

// ── shared worker plumbing (fd_refcount conventions) ──────────────────
static int bc_err;                       // sticky failure flag
static int bc_abort;                     // cooperative abort

static int aborted(void)
{
    return __atomic_load_n(&bc_abort, __ATOMIC_ACQUIRE);
}

static void fail_worker(void)
{
    __atomic_store_n(&bc_err, 1, __ATOMIC_RELEASE);
}

// Control gates must BLOCK: a runnable worker on the BSP prevents its
// idle harness from resuming to publish the next command. The blocker
// callback also supplies a deadline/abort wake without fixture timers.
// Workers are pinned; each CPU has at most one active control gate.
static struct {
    const volatile int *flag;
    int expected;
    uint64_t deadline;
} bc_gate[NR_CPUS];

static bool control_ready(task_t *waiter)
{
    unsigned cpu = waiter->cpu;
    return __atomic_load_n(bc_gate[cpu].flag, __ATOMIC_ACQUIRE) >=
               bc_gate[cpu].expected ||
           __atomic_load_n(&bc_abort, __ATOMIC_ACQUIRE) ||
           jiffies >= bc_gate[cpu].deadline;
}

static int worker_wait_progress(const volatile int *flag, int expected, int *err)
{
    unsigned cpu = cpu_id();
    uint64_t deadline = jiffies + BC_WAIT_TICKS;
    arch_local_irq_enable();
    while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) < expected) {
        if (__atomic_load_n(&bc_abort, __ATOMIC_ACQUIRE))
            return 0;
        if (jiffies >= deadline) {
            if (err) *err = -ETIMEDOUT;
            __atomic_store_n(&bc_abort, 1, __ATOMIC_RELEASE);
            return 0;
        }
        bc_gate[cpu].flag = flag;
        bc_gate[cpu].expected = expected;
        bc_gate[cpu].deadline = deadline;
        blocker_wait(control_ready, BLOCKER_NANOSLEEP, false);
        arch_local_irq_enable();
    }
    return 1;
}

static int worker_wait(const volatile int *flag)
{
    return worker_wait_progress(flag, 1, NULL);
}

// Only the harness yields: it cannot sleep on a wait queue.
static int harness_wait_progress(const volatile int *flag, int expected)
{
    uint64_t start = jiffies;
    while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) < expected) {
        if (jiffies - start >= BC_WAIT_TICKS)
            return 0;
        arch_local_irq_enable();
        schedule();
        arch_local_irq_enable();
    }
    return 1;
}

// ── scenario 1: same-sector atomicity ─────────────────────────────────
static const uint8_t bc_pat_a = 0xA7;
static const uint8_t bc_pat_b = 0x5C;

static uint8_t  bc_atomic_wbuf[512], bc_atomic_rbuf[512];
static volatile int bc_atomic_wdone, bc_atomic_rdone;
static volatile int bc_atomic_wprogress, bc_atomic_rprogress;
static volatile int bc_atomic_mixed;
static int bc_atomic_reader_ready;
static unsigned bc_atomic_wcpu, bc_atomic_rcpu;
static int bc_atomic_reads;

#define BC_ATOMIC_ITERS 2000

static uint64_t atomic_writer(uint64_t arg)
{
    (void)arg;
    bc_atomic_wcpu = cpu_id();
    if (!worker_wait(&bc_atomic_reader_ready))
        goto out;
    for (int i = 0; i < BC_ATOMIC_ITERS && !aborted(); i++) {
        memset(bc_atomic_wbuf, (i & 1) ? bc_pat_b : bc_pat_a, 512);
        if (block_device_write(bc_dev, 3, 1, bc_atomic_wbuf) != 0)
            fail_worker();
        __atomic_store_n(&bc_atomic_wprogress, i + 1, __ATOMIC_RELEASE);
        // A real reader sample must acknowledge every batch, including
        // the last. A serial reader or early exit cannot satisfy this.
        if (((i & 15) == 15 || i + 1 == BC_ATOMIC_ITERS) &&
            !worker_wait_progress(&bc_atomic_rprogress, i + 1, NULL))
            break;
    }
out:
    __atomic_store_n(&bc_atomic_wdone, 1, __ATOMIC_RELEASE);
    return 0;
}

static uint64_t atomic_reader(uint64_t arg)
{
    (void)arg;
    bc_atomic_rcpu = cpu_id();
    __atomic_store_n(&bc_atomic_reader_ready, 1, __ATOMIC_RELEASE);
    if (!worker_wait_progress(&bc_atomic_wprogress, 1, NULL))
        goto out;
    while (!__atomic_load_n(&bc_atomic_wdone, __ATOMIC_ACQUIRE) && !aborted()) {
        int progress = __atomic_load_n(&bc_atomic_wprogress, __ATOMIC_ACQUIRE);
        if (block_device_read(bc_dev, 3, 1, bc_atomic_rbuf) != 0)
            fail_worker();
        uint8_t b0 = bc_atomic_rbuf[0];
        if (b0 != bc_pat_a && b0 != bc_pat_b)
            bc_atomic_mixed = 1;
        for (int i = 1; i < 512; i++)
            if (bc_atomic_rbuf[i] != b0) { bc_atomic_mixed = 1; break; }
        bc_atomic_reads++;
        __atomic_store_n(&bc_atomic_rprogress, progress, __ATOMIC_RELEASE);
    }
out:
    __atomic_store_n(&bc_atomic_rdone, 1, __ATOMIC_RELEASE);
    return 0;
}

// ── scenario 2: concurrent first miss on one key ──────────────────────
static uint8_t  bc_miss_buf1[512], bc_miss_buf2[512];
static volatile int bc_miss_r1_done, bc_miss_r2_done;
static volatile int bc_miss_r1_go, bc_miss_r2_go;

static uint64_t miss_reader1(uint64_t arg)
{
    (void)arg;
    if (worker_wait(&bc_miss_r1_go) &&
        block_device_read(bc_dev, BC_KEY_STALL, 1, bc_miss_buf1) != 0)
        fail_worker();
    __atomic_store_n(&bc_miss_r1_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static uint64_t miss_reader2(uint64_t arg)
{
    (void)arg;
    if (worker_wait(&bc_miss_r2_go) &&
        block_device_read(bc_dev, BC_KEY_STALL, 1, bc_miss_buf2) != 0)
        fail_worker();
    __atomic_store_n(&bc_miss_r2_done, 1, __ATOMIC_RELEASE);
    return 0;
}

// ── scenario 3: adjacent-sector isolation ─────────────────────────────
static uint8_t  bc_adj_buf[2][512];
static volatile int bc_adj_done[2];

static uint64_t adjacent_writer(uint64_t arg)
{
    unsigned which = (unsigned)arg;
    uint64_t lba = which ? 11 : 10;
    uint8_t pat = which ? 0x3C : 0xC3;
    uint8_t *buf = bc_adj_buf[which];
    for (int i = 0; i < 500 && !aborted(); i++) {
        memset(buf, pat, 512);
        if (block_device_write(bc_dev, lba, 1, buf) != 0)
            fail_worker();
    }
    memset(buf, 0, 512);
    if (block_device_read(bc_dev, lba, 1, buf) != 0)
        fail_worker();
    for (int i = 0; i < 512; i++)
        if (buf[i] != pat) { fail_worker(); break; }
    __atomic_store_n(&bc_adj_done[which], 1, __ATOMIC_RELEASE);
    return 0;
}

// ── scenario 4: flush waits behind a held pin ─────────────────────────
static uint8_t  bc_flush_buf[512];
static volatile int bc_flush_pinned, bc_flush_unpin_req, bc_flush_unpinned;
static volatile int bc_flush_flush_go, bc_flush_flusher_done;
static volatile int bc_flush_ret = 1234;

static uint64_t flush_pin_worker(uint64_t arg)
{
    (void)arg;
    // write through the cache, then hold a pin on the dirty key
    memset(bc_flush_buf, 0x74, 512);
    if (block_device_write(bc_dev, BC_KEY_FLUSH, 1, bc_flush_buf) != 0)
        fail_worker();
    int pret = bcache_debug_pin(bc_dev, BC_KEY_FLUSH);
    if (pret != 0)
        fail_worker();
    __atomic_store_n(&bc_flush_pinned, 1, __ATOMIC_RELEASE);
    worker_wait(&bc_flush_unpin_req);
    if (pret == 0)
        bcache_debug_unpin(bc_dev, BC_KEY_FLUSH);
    __atomic_store_n(&bc_flush_unpinned, 1, __ATOMIC_RELEASE);
    return 0;
}

static uint64_t flush_flusher_worker(uint64_t arg)
{
    (void)arg;
    bc_flush_ret = worker_wait(&bc_flush_flush_go) ?
                       block_device_flush(bc_dev) : -EINTR;
    __atomic_store_n(&bc_flush_flusher_done, 1, __ATOMIC_RELEASE);
    return 0;
}

// ── scenario 5: exhaustion wait ───────────────────────────────────────
static volatile int bc_exh_pinned, bc_exh_unpin_req, bc_exh_unpinned;
static volatile int bc_exh_read_go, bc_exh_read_done, bc_exh_read_ret = 1234;
static volatile int bc_exh_cleanup_req, bc_exh_pinner_done;
static uint8_t  bc_exh_buf[512];

static uint64_t exh_pinner_worker(uint64_t arg)
{
    (void)arg;
    uint32_t pinned = 0;
    for (; pinned < BCACHE_BUFS && !aborted(); pinned++) {
        if (bcache_debug_pin(bc_dev, pinned) != 0) { fail_worker(); break; }
    }
    __atomic_store_n(&bc_exh_pinned, 1, __ATOMIC_RELEASE);
    worker_wait(&bc_exh_unpin_req);
    if (pinned > 5)
        bcache_debug_unpin(bc_dev, 5);      // free exactly one slot
    __atomic_store_n(&bc_exh_unpinned, 1, __ATOMIC_RELEASE);
    worker_wait(&bc_exh_cleanup_req);
    // Abort must release every successfully acquired pin as well.
    for (uint32_t i = 0; i < pinned; i++) {
        if (i == 5) continue;
        bcache_debug_unpin(bc_dev, i);
    }
    __atomic_store_n(&bc_exh_pinner_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static uint64_t exh_reader_worker(uint64_t arg)
{
    (void)arg;
    bc_exh_read_ret = worker_wait(&bc_exh_read_go) ?
                         block_device_read(bc_dev, BC_KEY_EXH, 1, bc_exh_buf) : -EINTR;
    __atomic_store_n(&bc_exh_read_done, 1, __ATOMIC_RELEASE);
    return 0;
}

// ── harness lifecycle and evidence ──────────────────────────────────
static int worker_pid[2] = { -1, -1 };
static const volatile int *worker_done[2];

static int start_pair(uint64_t (*fn0)(uint64_t), uint64_t arg0,
                      const volatile int *done0, unsigned cpu0,
                      uint64_t (*fn1)(uint64_t), uint64_t arg1,
                      const volatile int *done1, unsigned cpu1)
{
    worker_done[0] = done0;
    worker_done[1] = done1;
    worker_pid[0] = kernel_thread_on_cpu(fn0, arg0, cpu0);
    worker_pid[1] = kernel_thread_on_cpu(fn1, arg1, cpu1);
    return worker_pid[0] >= 0 && worker_pid[1] >= 0;
}

// Each done publication is the worker's LAST fixture access. Do not reuse
// its flags/buffers or retire the fake device until all created workers
// have published done (including a partial create or timeout).
static int join_pair(void)
{
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        if (worker_pid[i] < 0) continue;
        if (harness_wait_progress(worker_done[i], 1))
            worker_pid[i] = -1;
        else
            ok = 0;
    }
    return ok;
}

// Safe observation: self-reaping workers may disappear as soon as they
// finish. Find by PID under task_list_lock, then snapshot state under its
// rq_lock; never retain a raw task pointer after dropping these locks.
static int cache_waiter_asleep(int pid)
{
    int asleep = 0;
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    list_t *head = &init_task_union.task.list;
    for (list_t *pos = task_list_next(head); pos && pos != head;
         pos = task_list_next(pos)) {
        task_t *t = container_of(pos, task_t, list);
        if (t->pid != pid) continue;
        unsigned cpu = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
        uint64_t rqflags = spin_lock_irqsave(&percpu_data[cpu].rq_lock);
        asleep = t->state == TASK_INTERRUPTIBLE && !t->on_cpu &&
                 t->blocker.type == BLOCKER_NONE;
        spin_unlock_irqrestore(&percpu_data[cpu].rq_lock, rqflags);
        break;
    }
    spin_unlock_irqrestore(&task_list_lock, flags);
    return asleep;
}

static int wait_for_cache_sleep(int pid)
{
    uint64_t start = jiffies;
    while (jiffies - start < BC_WAIT_TICKS && !aborted()) {
        bcache_test_stats_t st;
        bcache_debug_stats(bc_dev, &st);
        if (st.waiters && cache_waiter_asleep(pid))
            return 1;
        arch_local_irq_enable();
        schedule();
        arch_local_irq_enable();
    }
    return 0;
}

static int sector_is(const uint8_t *buf, uint8_t pattern)
{
    for (unsigned i = 0; i < BCACHE_SECTOR; i++)
        if (buf[i] != pattern) return 0;
    return 1;
}

static int workers_ok(void)
{
    return !aborted() && !__atomic_load_n(&bc_err, __ATOMIC_ACQUIRE);
}

// ── main entry (late case) ────────────────────────────────────────────
int test_bcache_smp_coherence(void);

int test_bcache_smp_coherence(void)
{
    arch_irq_state_t flags = arch_local_irq_save();
    unsigned hcpu = cpu_id(), peer;
    for (peer = 0; peer < num_cpus && peer < NR_CPUS; peer++)
        if (peer != hcpu && percpu_data[peer].online &&
            percpu_data[peer].scheduler_ok)
            break;
    if (peer >= num_cpus || peer >= NR_CPUS) {
        serial_printk("[selftest] bcache_smp_coherence FAIL (requires two scheduler-ready CPUs)\n");
        arch_local_irq_restore(flags);
        return -1;
    }

    int rc = -1;
    bc_err = bc_abort = 0;
    worker_pid[0] = worker_pid[1] = -1;
    bc_stall_key = -1;
    for (uint32_t i = 0; i < BC_SECTORS; i++)
        memset(bc_backend[i], bc_pat_a, BCACHE_SECTOR);
    struct block_device_desc desc = {
        .name = "bcachetest", .sector_count = BC_SECTORS,
        .sector_size = BCACHE_SECTOR, .ops = &bc_ops, .kind = BLOCK_DISK,
    };
    if (block_device_register(&desc, &bc_dev) != 0 || !bc_dev) {
        serial_printk("[selftest] bcache_smp_coherence FAIL (register)\n");
        arch_local_irq_restore(flags);
        return -1;
    }

    serial_printk("[selftest] bcache same_sector_atomic... ");
    bc_atomic_wdone = bc_atomic_rdone = bc_atomic_reader_ready = 0;
    bc_atomic_wprogress = bc_atomic_rprogress = bc_atomic_reads = 0;
    bc_atomic_mixed = 0;
    if (!start_pair(atomic_writer, 0, &bc_atomic_wdone, hcpu,
                    atomic_reader, 0, &bc_atomic_rdone, peer) ||
        !join_pair() || !workers_ok() || bc_atomic_mixed ||
        bc_atomic_wprogress != BC_ATOMIC_ITERS ||
        bc_atomic_rprogress != BC_ATOMIC_ITERS ||
        bc_atomic_wcpu != hcpu || bc_atomic_rcpu != peer)
        goto cleanup;
    serial_printk("PASS (writes=%d reads=%d acknowledged=%d cpus=%u/%u)\n",
                  bc_atomic_wprogress, bc_atomic_reads, bc_atomic_rprogress, hcpu, peer);

    serial_printk("[selftest] bcache same_sector_concurrent_miss... ");
    bc_stall_key = BC_KEY_STALL;
    bc_stall_entered = bc_stall_release = bc_stall_consumed = 0;
    bc_miss_r1_done = bc_miss_r2_done = bc_miss_r1_go = bc_miss_r2_go = 0;
    int reads_before = __atomic_load_n(&bc_reads, __ATOMIC_RELAXED);
    if (!start_pair(miss_reader1, 0, &bc_miss_r1_done, hcpu,
                    miss_reader2, 0, &bc_miss_r2_done, peer))
        goto cleanup;
    __atomic_store_n(&bc_miss_r1_go, 1, __ATOMIC_RELEASE);
    if (!harness_wait_progress(&bc_stall_entered, 1)) goto cleanup;
    __atomic_store_n(&bc_miss_r2_go, 1, __ATOMIC_RELEASE);
    if (!wait_for_cache_sleep(worker_pid[1])) goto cleanup;
    __atomic_store_n(&bc_stall_release, 1, __ATOMIC_RELEASE);
    if (!join_pair() || !workers_ok() ||
        __atomic_load_n(&bc_reads, __ATOMIC_RELAXED) - reads_before != 1 ||
        !sector_is(bc_miss_buf1, bc_pat_a) || !sector_is(bc_miss_buf2, bc_pat_a))
        goto cleanup;
    bc_stall_key = -1;
    serial_printk("PASS (backend_reads=1 slept=1)\n");

    serial_printk("[selftest] bcache adjacent_sector_isolation... ");
    bc_adj_done[0] = bc_adj_done[1] = 0;
    if (!start_pair(adjacent_writer, 0, &bc_adj_done[0], hcpu,
                    adjacent_writer, 1, &bc_adj_done[1], peer) ||
        !join_pair() || !workers_ok())
        goto cleanup;
    serial_printk("PASS\n");

    serial_printk("[selftest] bcache flush_waits_for_reader... ");
    bc_flush_pinned = bc_flush_unpin_req = bc_flush_unpinned = 0;
    bc_flush_flush_go = bc_flush_flusher_done = 0;
    bc_flush_ret = 1234;
    if (!start_pair(flush_pin_worker, 0, &bc_flush_unpinned, hcpu,
                    flush_flusher_worker, 0, &bc_flush_flusher_done, peer) ||
        !harness_wait_progress(&bc_flush_pinned, 1))
        goto cleanup;
    bcache_test_stats_t st;
    bcache_debug_stats(bc_dev, &st);
    if (st.pinned != 1 || !st.dirty) goto cleanup;
    __atomic_store_n(&bc_flush_flush_go, 1, __ATOMIC_RELEASE);
    if (!wait_for_cache_sleep(worker_pid[1]) || bc_flush_flusher_done)
        goto cleanup;
    __atomic_store_n(&bc_flush_unpin_req, 1, __ATOMIC_RELEASE);
    if (!join_pair() || !workers_ok() || bc_flush_ret ||
        !sector_is(bc_backend[BC_KEY_FLUSH], 0x74))
        goto cleanup;
    serial_printk("PASS (slept=1 drained=1)\n");

    serial_printk("[selftest] bcache exhaustion_waits... ");
    bc_exh_pinned = bc_exh_unpin_req = bc_exh_unpinned = 0;
    bc_exh_read_go = bc_exh_read_done = 0;
    bc_exh_cleanup_req = bc_exh_pinner_done = 0;
    bc_exh_read_ret = 1234;
    if (!start_pair(exh_pinner_worker, 0, &bc_exh_pinner_done, hcpu,
                    exh_reader_worker, 0, &bc_exh_read_done, peer) ||
        !harness_wait_progress(&bc_exh_pinned, 1))
        goto cleanup;
    bcache_debug_stats(bc_dev, &st);
    if (st.entries != BCACHE_BUFS || st.pinned != BCACHE_BUFS)
        goto cleanup;
    __atomic_store_n(&bc_exh_read_go, 1, __ATOMIC_RELEASE);
    if (!wait_for_cache_sleep(worker_pid[1]) || bc_exh_read_done)
        goto cleanup;
    __atomic_store_n(&bc_exh_unpin_req, 1, __ATOMIC_RELEASE);
    if (!harness_wait_progress(&bc_exh_unpinned, 1) ||
        !harness_wait_progress(&bc_exh_read_done, 1))
        goto cleanup;
    __atomic_store_n(&bc_exh_cleanup_req, 1, __ATOMIC_RELEASE);
    if (!join_pair() || !workers_ok() || bc_exh_read_ret ||
        !sector_is(bc_exh_buf, bc_pat_a))
        goto cleanup;
    serial_printk("PASS (pinned=%d slept=1 released=1 completed=1)\n", BCACHE_BUFS);

    serial_printk("[selftest] bcache flush_backend_visibility... ");
    bc_flush_flusher_done = 0;
    worker_done[0] = &bc_flush_flusher_done;
    worker_pid[0] = kernel_thread_on_cpu(flush_flusher_worker, 0, peer);
    // The release flag from the earlier flush remains set.
    if (worker_pid[0] < 0 || !join_pair() || !workers_ok() || bc_flush_ret ||
        !sector_is(bc_backend[10], 0xC3) || !sector_is(bc_backend[11], 0x3C) ||
        !sector_is(bc_backend[BC_KEY_FLUSH], 0x74) ||
        !sector_is(bc_backend[BC_KEY_STALL], bc_pat_a) ||
        !sector_is(bc_backend[BC_KEY_EXH], bc_pat_a))
        goto cleanup;
    serial_printk("PASS (full sectors backend visible)\n");
    rc = 0;

cleanup:
    if (rc) {
        __atomic_store_n(&bc_abort, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&bc_stall_release, 1, __ATOMIC_RELEASE);
        serial_printk("FAIL (abort=%d err=%d)\n", aborted(), bc_err);
    }
    // Blocker callbacks see abort; the pinner always releases the exact
    // acquired pin set. Never advance another scenario on failure.
    if (!join_pair()) {
        serial_printk("[selftest] bcache_smp_coherence FAIL (workers still active; fixture retained)\n");
        arch_local_irq_restore(flags);
        return -1;
    }
    bcache_debug_stats(bc_dev, &st);
    if (st.pinned || st.busy || st.waiters || (!rc && st.dirty)) {
        serial_printk("[selftest] bcache no_leaks FAIL (pinned=%u busy=%u waiters=%u dirty=%u)\n",
                      st.pinned, st.busy, st.waiters, st.dirty);
        rc = -1;                         // retain fixture if it is still in use
    } else {
        if (block_device_unregister_boot(bc_dev) != 0)
            rc = -1;
        bcache_debug_stats(bc_dev, &st);
        if (st.entries) rc = -1;
        else serial_printk("[selftest] bcache no_leaks PASS\n");
        bc_dev = NULL;
    }
    serial_printk("[selftest] bcache_smp_coherence %s\n", rc ? "FAIL" : "PASS");
    arch_local_irq_restore(flags);
    return rc;
}

#endif // OS01_SELFTEST && !__aarch64__
