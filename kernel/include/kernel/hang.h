#ifndef _KERNEL_HANG_H
#define _KERNEL_HANG_H

// ── Hang detector ─────────────────────────────────────────
// If a CPU's watchdog_counter reaches HANG_THRESHOLD, the
// scheduler dumps all task state and stack traces to serial.
#define HANG_THRESHOLD  50   // 50 ticks without schedule() = 500 ms at 100 Hz

// Dump all tasks and stacks to serial.  Called from schedule()
// when watchdog_counter >= HANG_THRESHOLD.
void hang_dump_all(void);

#endif // _KERNEL_HANG_H
