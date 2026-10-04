#include <syscall/dispatch.h>
#include <uapi/syscall.h>
#include <errno.h>
#include <sched/task.h>
#include <memory/uaccess.h>
#include <uapi/time.h>
#include <arch/clocksource.h>
#include <string.h>

// ── nanosleep blocker condition ────────────────────────────
// Condition callback for blocker_wait(): true once the sleep deadline
// (current->wakeup_ns) has been reached.  sched_unblock_blocked()
// runs this from every schedule() (i.e. every tick) and wakes the
// sleeping task when it returns true.
static bool nanosleep_should_unblock(struct task_struct *waiter)
{
    return arch_clocksource_read_ns() >= waiter->wakeup_ns;
}

int64_t sys_time_dispatch(syscall_ctx_t *ctx)
{
    int64_t syscall_result = -EINVAL;
    switch (ctx->nr) {
    case SYS_time: {
        // time(time_t *tloc) → 0 (Jan 1 1970 for MVP).
        // NULL → skip; otherwise copy_to_user_ft writes 0 to tloc.
        uint64_t *tloc = (uint64_t *)ctx->args[0];
        if (tloc) {
            uint64_t zero = 0;
            ssize_t r = copy_to_user_ft(tloc, &zero, sizeof(zero));
            if (r < 0) { syscall_result = r; break; }
        }
        syscall_result = 0;
        break;
    }
    case SYS_gettimeofday: {
        // gettimeofday(struct timeval *tv, struct timezone *tz) → 0.
        // Both pointers may be NULL (POSIX).
        struct timeval *tv = (struct timeval *)ctx->args[0];
        struct timezone *tz = (struct timezone *)ctx->args[1];
        if (tv) {
            struct timeval ktv = { 0, 0 };
            ssize_t r = copy_to_user_ft(tv, &ktv, sizeof(ktv));
            if (r < 0) { syscall_result = r; break; }
        }
        if (tz) {
            struct timezone ktz = { 0, 0 };
            ssize_t r = copy_to_user_ft(tz, &ktz, sizeof(ktz));
            if (r < 0) { syscall_result = r; break; }
        }
        syscall_result = 0;
        break;
    }
    case SYS_clock_gettime: {
        // clock_gettime(clockid_t clk_id, struct timespec *tp)
        // OS01 has no real RTC wall clock yet (gettimeofday returns 0),
        // so both CLOCK_REALTIME and CLOCK_MONOTONIC report the same
        // monotonic clocksource time (clocksource_read_ns, ns).
        uint64_t clk_id = ctx->args[0];
        struct timespec *tp = (struct timespec *)ctx->args[1];
        if (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC) {
            syscall_result = -EINVAL;
            break;
        }
        if (!tp) {
            syscall_result = -EFAULT;
            break;
        }
        uint64_t ns = arch_clocksource_read_ns();
        struct timespec kts = {
            .tv_sec  = ns / 1000000000ULL,
            .tv_nsec = ns % 1000000000ULL,
        };
        ssize_t r = copy_to_user_ft(tp, &kts, sizeof(kts));
        if (r < 0) { syscall_result = r; break; }
        syscall_result = 0;
        break;
    }
    case SYS_nanosleep: {
        // nanosleep(const struct timespec *req, struct timespec *rem).
        // req → kernel copy; rem ← kernel copy on -EINTR (NULL legal).
        const struct timespec *req = (const struct timespec *)ctx->args[0];
        struct timespec *rem = (struct timespec *)ctx->args[1];
        uint64_t ns = 0;
        if (req) {
            // entry fast reject: 16 B must be mapped readable
            if (!syscall_check_user_range((uint64_t)req, sizeof(*req), false)) {
                syscall_result = -EFAULT;
                break;
            }
            struct timespec kreq;
            if (copy_from_user_ft(&kreq, req, sizeof(kreq)) < 0) {
                syscall_result = -EFAULT;
                break;
            }
            ns = kreq.tv_sec * 1000000000ULL + kreq.tv_nsec;
        }

        uint64_t target_ns = arch_clocksource_read_ns() + ns;
        current->wakeup_ns = target_ns;

        // Real sleep via the blocker framework: sched_unblock_blocked()
        // (run from every schedule(), i.e. every tick) wakes us once
        // clocksource reaches target_ns.  The loop absorbs spurious wakes.
        int r;
        do {
            r = blocker_wait(nanosleep_should_unblock, BLOCKER_NANOSLEEP, true);
        } while (r == 0 && arch_clocksource_read_ns() < target_ns);
        current->wakeup_ns = 0;

        if (r == -EINTR) {
            // Interrupted by a signal before the deadline: report the
            // remaining time (guarded against unsigned underflow).
            uint64_t now_ns = arch_clocksource_read_ns();
            uint64_t remain_ns = (now_ns < target_ns) ? (target_ns - now_ns) : 0;
            if (rem) {
                struct timespec krem = {
                    .tv_sec  = remain_ns / 1000000000ULL,
                    .tv_nsec = remain_ns % 1000000000ULL,
                };
                ssize_t wr = copy_to_user_ft(rem, &krem, sizeof(krem));
                if (wr < 0) { syscall_result = wr; break; }
            }
            syscall_result = -EINTR;
        } else {
            syscall_result = 0;
        }
        break;
    }
    case SYS_times: {
        // times(struct tms *buf) — stub: return 0.
        // NULL → skip; otherwise zero-init user struct.
        struct tms *buf = (struct tms *)ctx->args[0];
        if (buf) {
            struct tms kbuf;
            memset(&kbuf, 0, sizeof(kbuf));
            ssize_t r = copy_to_user_ft(buf, &kbuf, sizeof(kbuf));
            if (r < 0) { syscall_result = r; break; }
        }
        syscall_result = 0;
        break;
    }
    default:
        break;
    }
    return syscall_result;
}
