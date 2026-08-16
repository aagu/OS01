// kernel/test/test_fd_refcount.c
// ── fd reference-protocol SMP race tests ──────────────────
// Two scenarios, run from task_init() AFTER scheduler_ok=1
// (kernel_thread + schedule() work).  files_unpin is now a
// synchronous drop-to-zero → files_free.
//
// Synchronisation uses __atomic acquire/release flags, NOT volatile:
// volatile gives no cross-CPU happens-before.  The harness runs as the
// idle task and cannot wait_queue_sleep (wait.c adds current->io_wait_node
// and schedule()s), so it spins on flags via schedule(); workers are
// kthreads and use the same spin, keeping one uniform protocol.
//
// Ownership: harness holds the initial files_t ref; each SUCCESSFULLY
// created worker files_pin()s its own ref and unpins BEFORE signaling done.
// An abort flag + start flag let a partially-created worker set exit cleanly
// instead of spinning forever.  A scenario PASSes only when it provably ran
// cross-CPU and (for R2) provably observed a detach inside the reader's
// active window.  A use-after-free otherwise manifests as a crash (#PF).

#if defined(OS01_SELFTEST)

#include <kernel/printk.h>
#include <kernel/task.h>
#include <kernel/file.h>
#include <kernel/percpu.h>

#define FD_RACE_ITERS  10000
#define SPIN_LIMIT     10000000

static void wait_flag(const int *flag)
{
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE))
        schedule();
}

// Returns 1 if flag observed set within SPIN_LIMIT, else 0 (timeout).
static int wait_flag_timeout(const int *flag)
{
    int spins = 0;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE) && spins < SPIN_LIMIT) {
        schedule();
        spins++;
    }
    return __atomic_load_n(flag, __ATOMIC_ACQUIRE) != 0;
}

// ── Scenario 1: get-vs-detach (R3) ────────────────────────
static files_t *race_fs;
static int      r3_start;
static int      r3_abort;
static int      r3_reader_done;
static int      r3_writer_done;
static int      r3_reader_cpu_mask;   // bitmask of CPUs the reader ran on
static int      r3_writer_cpu_mask;   // bitmask of CPUs the writer ran on
static int      r3_writer_err;
static int      r3_saw_present;       // reader got a non-NULL file ref
static int      r3_saw_absent;        // reader saw slot empty (writer detached)

static uint64_t race_reader(uint64_t arg)
{
    (void)arg;
    wait_flag(&r3_start);
    if (__atomic_load_n(&r3_abort, __ATOMIC_ACQUIRE))
        goto out;                       // partial-create: exit without racing
    for (int i = 0; i < FD_RACE_ITERS; i++) {
        __atomic_fetch_or(&r3_reader_cpu_mask, 1U << cpu_id(), __ATOMIC_RELAXED);
        file_t *g = files_get_file(race_fs, 0);
        if (g) {
            r3_saw_present = 1;         // we DID grab a live file ref
            files_put_file(g);
        } else {
            r3_saw_absent = 1;          // slot empty (writer detached it)
        }
    }
out:
    files_unpin(race_fs);               // drop reader's own ref BEFORE signaling
    __atomic_store_n(&r3_reader_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static uint64_t race_writer(uint64_t arg)
{
    (void)arg;
    wait_flag(&r3_start);
    if (__atomic_load_n(&r3_abort, __ATOMIC_ACQUIRE))
        goto out;
    for (int i = 0; i < FD_RACE_ITERS; i++) {
        __atomic_fetch_or(&r3_writer_cpu_mask, 1U << cpu_id(), __ATOMIC_RELAXED);
        file_t *f = file_alloc();
        if (!f) { r3_writer_err++; continue; }
        int fd = fd_alloc(race_fs, f);
        if (fd < 0) { file_put(f); r3_writer_err++; continue; }
        // Widen the window where the slot is non-NULL so the reader can
        // actually grab a live ref (not just see NULL after we detach).
        if ((i & 15) == 0)
            schedule();
        fd_close(race_fs, fd);
    }
out:
    files_unpin(race_fs);
    __atomic_store_n(&r3_writer_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static void run_get_detach_race(void)
{
    race_fs = files_alloc();            // harness ref: refcount == 1
    if (!race_fs) { serial_printk("FAIL (files_alloc)\n"); return; }

    r3_start = 0; r3_abort = 0; r3_reader_done = 0; r3_writer_done = 0;
    r3_reader_cpu_mask = 0; r3_writer_cpu_mask = 0;
    r3_writer_err = 0; r3_saw_present = 0; r3_saw_absent = 0;

    // Pin a worker ref ONLY when kernel_thread() returns a pid >= 0
    // (authoritative: pid < 0 means the task was NOT created).
    int reader_ok = 0, writer_ok = 0;
    int rpid = kernel_thread(race_reader, 0, PF_KTHREAD);
    if (rpid >= 0) { files_pin(race_fs); reader_ok = 1; }
    int wpid = kernel_thread(race_writer, 0, PF_KTHREAD);
    if (wpid >= 0) { files_pin(race_fs); writer_ok = 1; }

    if (!reader_ok || !writer_ok) {
        // Release any created worker with abort set so it exits (not spin
        // forever on r3_start); wait for it to drop its ref, then drop ours.
        __atomic_store_n(&r3_abort, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&r3_start, 1, __ATOMIC_RELEASE);
        if (reader_ok) wait_flag_timeout(&r3_reader_done);
        if (writer_ok) wait_flag_timeout(&r3_writer_done);
        files_unpin(race_fs);
        serial_printk("FAIL (kthread create)\n");
        return;
    }

    __atomic_store_n(&r3_start, 1, __ATOMIC_RELEASE);   // release both workers

    int rd = wait_flag_timeout(&r3_reader_done);
    int wd = wait_flag_timeout(&r3_writer_done);

    if (!rd || !wd) {
        // Timeout: a worker may still be running (finite loop → will exit on
        // its own and drop its ref).  Deliberately LEAK the harness ref rather
        // than free a table a live worker may touch.
        serial_printk("FAIL (timeout reader=%d writer=%d)\n", rd, wd);
        return;
    }

    int cross_cpu = (r3_reader_cpu_mask & r3_writer_cpu_mask) == 0
                    && r3_reader_cpu_mask != 0 && r3_writer_cpu_mask != 0;

    if (r3_writer_err)
        serial_printk("FAIL (writer_err=%d)\n", r3_writer_err);
    else if (!(r3_saw_present && r3_saw_absent))
        serial_printk("FAIL (no slot interaction present=%d absent=%d)\n",
                      r3_saw_present, r3_saw_absent);
    else if (!cross_cpu)
        serial_printk("FAIL (no cross-CPU overlap reader=%x writer=%x)\n",
                      r3_reader_cpu_mask, r3_writer_cpu_mask);
    else
        serial_printk("PASS (cross-CPU reader=%x writer=%x)\n",
                      r3_reader_cpu_mask, r3_writer_cpu_mask);

    files_unpin(race_fs);               // harness ref → refcount 0 → synchronous files_free
}

// ── Scenario 2: pin-vs-detach (R2) ────────────────────────
// holder inherits its files_t via kernel_thread (do_fork files_dup's init's
// table); we never create or override a table, so there is no leaked ref.
// Orchestration forces the detach to happen INSIDE the reader's active
// window: reader pins successfully (present), signals reader_started; holder
// waits for that, records its CPU, signals holder_entered, then do_exit().
// reader keeps looping until it sees a NULL pin (absent).  PASS requires
// present && absent && holder_entered && cross-CPU.
static int r2_reader_go;       // harness → reader: start
static int r2_reader_started;  // reader → holder: "I pinned at least once"
static int r2_holder_entered;  // holder → harness: "about to do_exit"
static int r2_reader_done;
static int r2_reader_cpu_mask; // bitmask of CPUs the reader ran on
static int r2_holder_cpu = -1; // holder runs do_exit() exactly once → single CPU
static int r2_saw_present;
static int r2_saw_absent;

static uint64_t race_holder(uint64_t arg)
{
    (void)arg;
    wait_flag(&r2_reader_started);      // wait until reader has pinned once
    r2_holder_cpu = (int)cpu_id();
    __atomic_store_n(&r2_holder_entered, 1, __ATOMIC_RELEASE);
    do_exit(0);   // detaches inherited files under task_list_lock; no return
    return 0;
}

static uint64_t pin_reader(uint64_t arg)
{
    int pid = (int)arg;
    wait_flag(&r2_reader_go);
    for (int i = 0; i < FD_RACE_ITERS && !(r2_saw_present && r2_saw_absent); i++) {
        __atomic_fetch_or(&r2_reader_cpu_mask, 1U << cpu_id(), __ATOMIC_RELAXED);
        files_t *fs = task_files_pin_by_pid(pid);
        if (fs) {
            r2_saw_present = 1;
            files_unpin(fs);
            // Signal holder on the FIRST successful pin (may fire repeatedly;
            // holder's wait_flag absorbs it harmlessly).
            __atomic_store_n(&r2_reader_started, 1, __ATOMIC_RELEASE);
        } else {
            r2_saw_absent = 1;          // holder detached while we were active
        }
    }
    __atomic_store_n(&r2_reader_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static void run_pin_detach_race(void)
{
    r2_reader_go = 0; r2_reader_started = 0; r2_holder_entered = 0;
    r2_reader_done = 0;
    r2_reader_cpu_mask = 0; r2_holder_cpu = -1;
    r2_saw_present = 0; r2_saw_absent = 0;

    int holder_pid = kernel_thread(race_holder, 0, PF_KTHREAD);
    if (holder_pid < 0) { serial_printk("FAIL (holder create)\n"); return; }

    int reader_pid = kernel_thread(pin_reader, (uint64_t)holder_pid, PF_KTHREAD);
    if (reader_pid < 0) {
        // Let holder proceed to exit (its inherited table frees on do_exit).
        // We never touch holder task_t* again.
        __atomic_store_n(&r2_reader_started, 1, __ATOMIC_RELEASE);
        serial_printk("FAIL (reader create)\n");
        return;
    }

    __atomic_store_n(&r2_reader_go, 1, __ATOMIC_RELEASE);   // release reader

    if (!wait_flag_timeout(&r2_reader_done)) {
        serial_printk("FAIL (timeout)\n");
        return;
    }

    int holder_entered = __atomic_load_n(&r2_holder_entered, __ATOMIC_ACQUIRE);
    int cross_cpu = 0;
    if (holder_entered) {
        // Read r2_holder_cpu only after acquiring holder_entered (release/
        // acquire pairing gives the happens-before for the holder's write).
        int holder_cpu = r2_holder_cpu;
        cross_cpu = (holder_cpu >= 0) &&
                    (r2_reader_cpu_mask & (1U << holder_cpu)) == 0;
    }

    if (!(r2_saw_present && r2_saw_absent))
        serial_printk("FAIL (no detach observed present=%d absent=%d)\n",
                      r2_saw_present, r2_saw_absent);
    else if (!holder_entered)
        serial_printk("FAIL (holder never entered)\n");
    else if (!cross_cpu)
        serial_printk("FAIL (no cross-CPU reader=%x holder=%d)\n",
                      r2_reader_cpu_mask, r2_holder_cpu);
    else
        serial_printk("PASS (cross-CPU reader=%x holder=%d)\n",
                      r2_reader_cpu_mask, r2_holder_cpu);
}

void test_fd_refcount(void)
{
    serial_printk("[selftest] fd_refcount get-vs-detach... ");
    run_get_detach_race();

    serial_printk("[selftest] fd_refcount pin-vs-detach... ");
    run_pin_detach_race();
}

#endif // OS01_SELFTEST
