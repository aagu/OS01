// ── kernel/arch/aarch64/cpu/idle.c ──────────────────────────────
//
// aarch64 idle loop. <sched/task.h> file-scope declares
//   thread_t init_thread = { ..., .rip = (uint64_t)idle_resume, ... };
// unconditionally — no #ifdef __aarch64__ guard — so the aarch64 kernel
// must provide the symbol at link time. On x86_64,
// kernel/arch/x86_64/intr/entry.S provides `idle_resume` as the entry
// point that returns from idle.
//
// Until the scheduler port lands (roadmap §P2 上下文切换统一), aarch64
// has no runnable queue to re-check and nothing schedules init_thread,
// so `wfi`-forever is the *correct* idle behavior, not a placeholder:
// each CPU sleeps in the wait-for-interrupt state and wakes for its
// CNTP tick / GIC interrupts. When the port brings need_resched-aware
// idle, this loop grows the re-entry into the scheduler exactly where
// the x86_64 `idle_resume` resumes it.

#include <stdint.h>

void idle_resume(void)
{
    for (;;) {
        __asm__ __volatile__("wfi" ::: "memory");
    }
}
