// fd reference-protocol SMP tests, called from task_init's BSP idle task.
// Workers are pinned BEFORE publication. A held reference spans a remote
// detach; stress readers stay active until the writer finishes. CPU masks
// alone are not evidence of overlapping lifetimes (migration/serial runs).
#if defined(OS01_SELFTEST)

#include <core/printk.h>
#include <sched/task.h>
#include <fs/file.h>
#include <percpu/percpu.h>
#include <arch/irq.h>
#include <time/timer.h>

#define FD_RACE_ITERS 10000
#define FD_WAIT_TICKS 500  // 5 seconds at the kernel's 100 Hz tick

// Peer workers have distinct CPUs, so polling cannot starve the producer.
// Keep IRQs enabled for timer deadlines and other runnable tasks. In
// particular, schedule() returns with IRQs OFF, not the caller's old state.
static int worker_wait_progress(const int *flag, int expected, int *abort)
{
    uint64_t start = jiffies;
    arch_local_irq_enable();
    while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) < expected) {
        if (__atomic_load_n(abort, __ATOMIC_ACQUIRE))
            return 0;
        if (jiffies - start >= FD_WAIT_TICKS) {
            __atomic_store_n(abort, 1, __ATOMIC_RELEASE);
            return 0;
        }
        arch_cpu_pause();
    }
    return !__atomic_load_n(abort, __ATOMIC_ACQUIRE);
}

static int worker_wait(const int *flag, int *abort)
{
    return worker_wait_progress(flag, 1, abort);
}

// Only the idle harness yields: it cannot sleep on a wait queue. A worker
// never waits for a new harness action after start, so it can finish even
// when the harness shares its CPU and is not runnable until workers exit.
static int wait_done(const int *flag)
{
    uint64_t start = jiffies;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE) &&
           jiffies - start < FD_WAIT_TICKS) {
        arch_local_irq_enable();
        schedule();
        arch_local_irq_enable();
    }
    return __atomic_load_n(flag, __ATOMIC_ACQUIRE) != 0;
}

// Scenario 1: files_get_file vs fd_close, including last slot-ref removal.
static files_t *race_fs;
static int r3_start, r3_abort;
static int r3_reader_done, r3_writer_done;
static unsigned r3_reader_cpu_mask, r3_writer_cpu_mask;
static int r3_writer_err, r3_saw_present, r3_saw_absent;
static int r3_first_published, r3_reader_sampled, r3_first_detached;
static int r3_reader_detached_sampled, r3_held_across_detach;
static int r3_reader_active, r3_writer_progress, r3_reader_progress;
static int r3_stress_done;

static uint64_t race_reader(uint64_t arg)
{
    (void)arg;
    file_t *held = NULL;
    if (!worker_wait(&r3_start, &r3_abort) ||
        !worker_wait(&r3_first_published, &r3_abort))
        goto out;
    __atomic_fetch_or(&r3_reader_cpu_mask, 1U << cpu_id(), __ATOMIC_RELAXED);
    held = files_get_file(race_fs, 0);
    r3_saw_present = held != NULL;
    __atomic_store_n(&r3_reader_sampled, 1, __ATOMIC_RELEASE);
    if (!worker_wait(&r3_first_detached, &r3_abort))
        goto out;

    // The slot ref is gone; our get must keep the file alive on this CPU.
    if (held && __atomic_load_n(&held->refcount, __ATOMIC_ACQUIRE) == 1)
        r3_held_across_detach = 1;
    file_t *f = files_get_file(race_fs, 0);
    r3_saw_absent = f == NULL;
    files_put_file(f);
    files_put_file(held);
    held = NULL;
    __atomic_store_n(&r3_reader_detached_sampled, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&r3_reader_active, 1, __ATOMIC_RELEASE);

    uint64_t start = jiffies;
    while (!__atomic_load_n(&r3_stress_done, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&r3_abort, __ATOMIC_ACQUIRE))
            break;
        if (jiffies - start >= FD_WAIT_TICKS) {
            __atomic_store_n(&r3_abort, 1, __ATOMIC_RELEASE);
            break;
        }
        __atomic_fetch_or(&r3_reader_cpu_mask, 1U << cpu_id(), __ATOMIC_RELAXED);
        // Acquire progress BEFORE getting a file: the acknowledged sample
        // must occur after an actual writer close inside our active window.
        int progress = __atomic_load_n(&r3_writer_progress, __ATOMIC_ACQUIRE);
        f = files_get_file(race_fs, 0);
        if (f) r3_saw_present = 1;
        else r3_saw_absent = 1;
        files_put_file(f);
        if (progress)
            __atomic_store_n(&r3_reader_progress, progress, __ATOMIC_RELEASE);
    }
out:
    files_put_file(held);
    files_unpin(race_fs);
    __atomic_store_n(&r3_reader_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static uint64_t race_writer(uint64_t arg)
{
    (void)arg;
    if (!worker_wait(&r3_start, &r3_abort))
        goto out;
    __atomic_fetch_or(&r3_writer_cpu_mask, 1U << cpu_id(), __ATOMIC_RELAXED);
    file_t *first = file_alloc();
    int first_fd = first ? fd_alloc(race_fs, first) : -1;
    if (first_fd < 0) {
        file_put(first);
        r3_writer_err++;
        __atomic_store_n(&r3_abort, 1, __ATOMIC_RELEASE);
        goto out;
    }
    __atomic_store_n(&r3_first_published, 1, __ATOMIC_RELEASE);
    int sampled = worker_wait(&r3_reader_sampled, &r3_abort);
    fd_close(race_fs, first_fd);  // also close on abort/timeout
    if (!sampled)
        goto out;
    __atomic_store_n(&r3_first_detached, 1, __ATOMIC_RELEASE);
    if (!worker_wait(&r3_reader_detached_sampled, &r3_abort) ||
        !worker_wait(&r3_reader_active, &r3_abort))
        goto out;

    for (int i = 0; i < FD_RACE_ITERS; i++) {
        if (__atomic_load_n(&r3_abort, __ATOMIC_ACQUIRE))
            break;
        __atomic_fetch_or(&r3_writer_cpu_mask, 1U << cpu_id(), __ATOMIC_RELAXED);
        file_t *f = file_alloc();
        int fd = f ? fd_alloc(race_fs, f) : -1;
        if (fd < 0) {
            file_put(f);
            r3_writer_err++;
            __atomic_store_n(&r3_abort, 1, __ATOMIC_RELEASE);
            break;
        }
        fd_close(race_fs, fd);
        __atomic_store_n(&r3_writer_progress, i + 1, __ATOMIC_RELEASE);
        // Require a reader operation after each batch, including the last.
        // Neither a serial reader nor one that quits after the first close
        // can satisfy all 625 acknowledgements. The batch itself races.
        if (((i & 15) == 15 || i + 1 == FD_RACE_ITERS) &&
            !worker_wait_progress(&r3_reader_progress, i + 1, &r3_abort))
            break;
    }
out:
    __atomic_store_n(&r3_stress_done, 1, __ATOMIC_RELEASE);
    files_unpin(race_fs);
    __atomic_store_n(&r3_writer_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static int run_get_detach_race(unsigned reader_cpu, unsigned writer_cpu)
{
    race_fs = files_alloc();  // harness owns the initial ref
    if (!race_fs) { serial_printk("FAIL (files_alloc)\n"); return -1; }
    r3_start = r3_abort = r3_reader_done = r3_writer_done = 0;
    r3_reader_cpu_mask = r3_writer_cpu_mask = r3_writer_err = 0;
    r3_saw_present = r3_saw_absent = r3_held_across_detach = 0;
    r3_first_published = r3_reader_sampled = r3_first_detached = 0;
    r3_reader_detached_sampled = r3_reader_active = 0;
    r3_writer_progress = r3_reader_progress = r3_stress_done = 0;

    // Reserve before publication: even a worker that times out before
    // start owns a ref. Failed creates return their unused reservation.
    files_pin(race_fs);
    int rpid = kernel_thread_on_cpu(race_reader, 0, reader_cpu);
    if (rpid < 0) files_unpin(race_fs);
    files_pin(race_fs);
    int wpid = kernel_thread_on_cpu(race_writer, 0, writer_cpu);
    if (wpid < 0) files_unpin(race_fs);
    int created = rpid >= 0 && wpid >= 0;
    if (!created)
        __atomic_store_n(&r3_abort, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&r3_start, 1, __ATOMIC_RELEASE);

    int rd = rpid < 0 || wait_done(&r3_reader_done);
    int wd = wpid < 0 || wait_done(&r3_writer_done);
    if (!rd || !wd) {
        __atomic_store_n(&r3_abort, 1, __ATOMIC_RELEASE);
        rd = rpid < 0 || wait_done(&r3_reader_done);
        wd = wpid < 0 || wait_done(&r3_writer_done);
    }
    if (!rd || !wd) {
        // Retain the harness ref if any worker is still using the table.
        serial_printk("FAIL (cleanup timeout reader=%d writer=%d)\n", rd, wd);
        return -1;
    }

    int rc = -1;
    if (!created) {
        serial_printk("FAIL (kthread create reader=%d writer=%d)\n", rpid, wpid);
    } else if (__atomic_load_n(&r3_abort, __ATOMIC_ACQUIRE) || r3_writer_err) {
        serial_printk("FAIL (abort=%d writer_err=%d progress=%d)\n",
                      r3_abort, r3_writer_err, r3_writer_progress);
    } else if (!(r3_saw_present && r3_saw_absent && r3_held_across_detach &&
                 r3_reader_progress == FD_RACE_ITERS && r3_writer_progress == FD_RACE_ITERS)) {
        serial_printk("FAIL (present=%d absent=%d held=%d overlap=%d progress=%d)\n",
                      r3_saw_present, r3_saw_absent, r3_held_across_detach,
                      r3_reader_progress, r3_writer_progress);
    } else if (r3_reader_cpu_mask != (1U << reader_cpu) ||
               r3_writer_cpu_mask != (1U << writer_cpu)) {
        serial_printk("FAIL (CPU placement reader=%x writer=%x)\n",
                      r3_reader_cpu_mask, r3_writer_cpu_mask);
    } else {
        serial_printk("PASS (cross-CPU reader=%x writer=%x held=1 overlap=1 closes=%d batches=625)\n",
                      r3_reader_cpu_mask, r3_writer_cpu_mask, r3_writer_progress);
        rc = 0;
    }
    files_unpin(race_fs);
    return rc;
}

// Scenario 2: task_files_pin_by_pid vs do_exit's table detach. The reader
// retains its first successful pin until a later lookup sees NULL. Thus
// table lifetime, not just two slot states, spans the remote task's exit.
static int r2_reader_go, r2_abort, r2_reader_started;
static int r2_holder_entered, r2_reader_done;
static unsigned r2_reader_cpu_mask;
static int r2_holder_cpu;
static int r2_saw_present, r2_saw_absent, r2_held_across_detach;

static uint64_t race_holder(uint64_t arg)
{
    (void)arg;
    worker_wait(&r2_reader_started, &r2_abort);
    r2_holder_cpu = (int)cpu_id();
    // No fixture accesses after this release, including the abort path.
    __atomic_store_n(&r2_holder_entered, 1, __ATOMIC_RELEASE);
    do_exit(0);
    return 0;
}

static uint64_t pin_reader(uint64_t arg)
{
    int pid = (int)arg;
    files_t *held = NULL;
    if (!worker_wait(&r2_reader_go, &r2_abort))
        goto out;
    __atomic_fetch_or(&r2_reader_cpu_mask, 1U << cpu_id(), __ATOMIC_RELAXED);
    held = task_files_pin_by_pid(pid);
    if (!held) {
        __atomic_store_n(&r2_abort, 1, __ATOMIC_RELEASE);
        goto out;
    }
    r2_saw_present = 1;
    __atomic_store_n(&r2_reader_started, 1, __ATOMIC_RELEASE);

    uint64_t start = jiffies;
    while (!__atomic_load_n(&r2_abort, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_or(&r2_reader_cpu_mask, 1U << cpu_id(), __ATOMIC_RELAXED);
        files_t *fs = task_files_pin_by_pid(pid);
        if (!fs) {
            r2_saw_absent = 1;
            r2_held_across_detach =
                __atomic_load_n(&held->refcount, __ATOMIC_ACQUIRE) >= 1;
            break;
        }
        files_unpin(fs);
        if (jiffies - start >= FD_WAIT_TICKS) {
            __atomic_store_n(&r2_abort, 1, __ATOMIC_RELEASE);
            break;
        }
        arch_cpu_pause();
    }
out:
    files_unpin(held);
    __atomic_store_n(&r2_reader_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static int run_pin_detach_race(unsigned reader_cpu, unsigned holder_cpu)
{
    r2_reader_go = r2_abort = r2_reader_started = 0;
    r2_holder_entered = r2_reader_done = r2_reader_cpu_mask = 0;
    r2_holder_cpu = -1;
    r2_saw_present = r2_saw_absent = r2_held_across_detach = 0;

    int hpid = kernel_thread_on_cpu(race_holder, 0, holder_cpu);
    if (hpid < 0) { serial_printk("FAIL (holder create)\n"); return -1; }
    int rpid = kernel_thread_on_cpu(pin_reader, (uint64_t)hpid, reader_cpu);
    if (rpid < 0)
        __atomic_store_n(&r2_abort, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&r2_reader_go, 1, __ATOMIC_RELEASE);

    int rd = rpid < 0 || wait_done(&r2_reader_done);
    int hd = wait_done(&r2_holder_entered);
    if (!rd || !hd) {
        __atomic_store_n(&r2_abort, 1, __ATOMIC_RELEASE);
        rd = rpid < 0 || wait_done(&r2_reader_done);
        hd = wait_done(&r2_holder_entered);
    }
    if (!rd || !hd) {
        serial_printk("FAIL (cleanup timeout reader=%d holder=%d)\n", rd, hd);
        return -1;
    }
    if (rpid < 0) {
        serial_printk("FAIL (reader create)\n");
        return -1;
    }
    if (__atomic_load_n(&r2_abort, __ATOMIC_ACQUIRE) ||
        !(r2_saw_present && r2_saw_absent && r2_held_across_detach)) {
        serial_printk("FAIL (abort=%d present=%d absent=%d held=%d)\n",
                      r2_abort, r2_saw_present, r2_saw_absent, r2_held_across_detach);
        return -1;
    }
    if (r2_reader_cpu_mask != (1U << reader_cpu) ||
        r2_holder_cpu != (int)holder_cpu) {
        serial_printk("FAIL (CPU placement reader=%x holder=%d)\n",
                      r2_reader_cpu_mask, r2_holder_cpu);
        return -1;
    }
    serial_printk("PASS (cross-CPU reader=%x holder=%d held=1)\n",
                  r2_reader_cpu_mask, r2_holder_cpu);
    return 0;
}

int test_fd_refcount(void)
{
    arch_irq_state_t flags = arch_local_irq_save();
    unsigned reader_cpu = cpu_id(), peer;
    for (peer = 0; peer < num_cpus && peer < NR_CPUS; peer++) {
        if (peer != reader_cpu && percpu_data[peer].online &&
            percpu_data[peer].scheduler_ok)
            break;
    }
    if (peer >= num_cpus || peer >= NR_CPUS) {
        serial_printk("[selftest] fd_refcount FAIL (requires two scheduler-ready CPUs)\n");
        arch_local_irq_restore(flags);
        return -1;  // never claim SMP coverage on one CPU
    }

    serial_printk("[selftest] fd_refcount get-vs-detach... ");
    int r3 = run_get_detach_race(reader_cpu, peer);
    serial_printk("[selftest] fd_refcount pin-vs-detach... ");
    int r2 = run_pin_detach_race(reader_cpu, peer);
    arch_local_irq_restore(flags);
    return r3 || r2 ? -1 : 0;
}

#endif // OS01_SELFTEST
