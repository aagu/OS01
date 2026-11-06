/* Host-only runtime surface for compiling the REAL
 * kernel/arch/aarch64/platform/time.c (aarch64 per-CPU tick accounting
 * plan, Task 1: freeze the CNTP configuration + arm the local counter).
 *
 * The mock replaces ONLY the machine / GIC / DTB / IRQ / log boundaries:
 *
 *   - the CNTP register accesses (counter-frequency read, TVAL write,
 *     CTL read/write) and the instruction barrier,
 *   - the GIC handler registration,
 *   - the DTB-provided CNTP PPI,
 *   - the global tick_handler() entry point,
 *   - the log_* output.
 *
 * Every piece of ALGORITHM stays in the production object: the
 * frequency / period / PPI validation, the frozen-configuration
 * publish/acquire protocol, the TVAL -> CTL -> ISB -> read-back arm
 * order and the ENABLE-clearing failure path.  The mock never
 * re-implements any of them — it only records what the production
 * code did and lets the fixture inject register / GIC faults.
 *
 * Two compilation modes:
 *   - PRODUCTION OBJECT (kernel/arch/aarch64/platform/time.c): this
 *     header is the only preinclude; the whole control surface below
 *     is declared extern.
 *   - TEST TU (hosttests/cases/test_aarch64_sched_tick.c): the
 *     framework preincludes test_platform.h FIRST (defining
 *     TEST_PLATFORM_H); this header then DEFINES the control surface
 *     and the mocked boundary functions.
 */
#ifndef AARCH64_SCHED_TICK_RUNTIME_H
#define AARCH64_SCHED_TICK_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Skip the real machine / GIC / DTB / clockevent / log headers ── */
#define _ARCH_AARCH64_GIC_H      /* <arch/aarch64/reg.h>   CNTP accessors  */
#define OS01_AARCH64_GIC_H       /* <arch/aarch64/gic.h>   handler registry */
#define OS01_AARCH64_DTB_H       /* <arch/aarch64/dtb.h>   CNTP PPI        */
#define _KERNEL_CLOCKEVENT_H     /* <time/clockevent.h>    tick_handler    */
#define _KERNEL_LOG_H            /* <log/log.h>             log_*           */
#define _ARCH_CPU_H              /* <arch/cpu.h>            arch facade     */

/* The public timer interface is host-safe and NOT skipped: the
 * production object and the fixture must agree on the frozen-config
 * layout and on the nominal tick rate. */
#include <arch/aarch64/timer.h>

/* ── Machine boundary ────────────────────────────────────────── */

struct pt_regs;

typedef void (*gic_handler_fn)(uint32_t intid, uint64_t param,
                               struct pt_regs *regs);

/* BSP boot start of the arch tick source (the <arch/cpu.h> facade
 * entry point that the real backend implements). */
bool arch_tick_start(void);

uint64_t cntfrq_el0(void);
void     cntp_tval_el0_write(uint64_t v);
uint64_t cntp_ctl_el0_read(void);
void     cntp_ctl_el0_write(uint64_t v);
void     cntp_isb(void);

int      gic_register_handler(uint32_t intid, gic_handler_fn fn,
                              uint64_t param, const char *name);
uint32_t dtb_cntp_ppi(void);
void     tick_handler(void);

/* ── Log surface ─────────────────────────────────────────────── */

void aarch64_timer_mock_log(const char *level, const char *fmt, ...);
#define log_err(...)  aarch64_timer_mock_log("err", __VA_ARGS__)
#define log_warn(...) aarch64_timer_mock_log("warn", __VA_ARGS__)
#define log_info(...) aarch64_timer_mock_log("info", __VA_ARGS__)

/* ── Register trace ──────────────────────────────────────────── */

enum aarch64_timer_op {
    AARCH64_TIMER_OP_FREQ_READ = 0,
    AARCH64_TIMER_OP_TVAL_WRITE,
    AARCH64_TIMER_OP_CTL_READ,
    AARCH64_TIMER_OP_CTL_WRITE,
    AARCH64_TIMER_OP_ISB,
};

#define AARCH64_TIMER_TRACE_MAX 64

struct aarch64_timer_trace_entry {
    enum aarch64_timer_op op;
    uint64_t value;
};

extern struct aarch64_timer_trace_entry
    aarch64_timer_trace[AARCH64_TIMER_TRACE_MAX];
extern int aarch64_timer_trace_count;

/* ── Fixture control surface ─────────────────────────────────── */

extern uint64_t aarch64_timer_mock_freq;      /* cntfrq_el0() result        */
extern uint64_t aarch64_timer_mock_ctl;       /* CNTP_CTL_EL0 register value */
extern uint64_t aarch64_timer_mock_tval;      /* CNTP_TVAL_EL0 register value*/
extern int      aarch64_timer_mock_readback_fails; /* read-back hides ENABLE   */
extern int      aarch64_timer_mock_imask_sticky;  /* CTL write keeps IMASK set*/
extern int      aarch64_timer_mock_register_rc;   /* forced gic_register rc   */
extern int      aarch64_timer_mock_register_calls;
extern uint32_t aarch64_timer_mock_register_intid;
extern uint32_t aarch64_timer_mock_ppi;       /* dtb_cntp_ppi() result      */
extern int      aarch64_timer_mock_tick_calls;

#define AARCH64_TIMER_MOCK_MAX_INTIDS 8
extern uint32_t aarch64_timer_mock_registered[AARCH64_TIMER_MOCK_MAX_INTIDS];
extern int      aarch64_timer_mock_registered_count;

/* ── Captured log lines ──────────────────────────────────────── */

#define AARCH64_TIMER_LOG_MAX 32
#define AARCH64_TIMER_LOG_LEN 256

extern char aarch64_timer_log_buf[AARCH64_TIMER_LOG_MAX][AARCH64_TIMER_LOG_LEN];
extern int  aarch64_timer_log_count;
extern int  aarch64_timer_log_info_count;
extern int  aarch64_timer_log_err_count;

/* Reset the fixture to a pristine state (registers, injected faults,
 * GIC registration table, trace and captured log lines).  The
 * PRODUCTION configuration published by the real backend is NOT
 * resettable — see the case-ordering note in the test TU. */
void aarch64_timer_mock_reset(void);

#ifdef TEST_PLATFORM_H
/* ══ Test TU: the whole control/spy layer is DEFINED here ════════ */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct aarch64_timer_trace_entry
    aarch64_timer_trace[AARCH64_TIMER_TRACE_MAX];
int aarch64_timer_trace_count;

uint64_t aarch64_timer_mock_freq = 62500000ULL;
uint64_t aarch64_timer_mock_ctl;
uint64_t aarch64_timer_mock_tval;
int      aarch64_timer_mock_readback_fails;
int      aarch64_timer_mock_imask_sticky;
int      aarch64_timer_mock_register_rc;
int      aarch64_timer_mock_register_calls;
uint32_t aarch64_timer_mock_register_intid;
uint32_t aarch64_timer_mock_ppi = 30U;
int      aarch64_timer_mock_tick_calls;

uint32_t aarch64_timer_mock_registered[AARCH64_TIMER_MOCK_MAX_INTIDS];
int      aarch64_timer_mock_registered_count;

char aarch64_timer_log_buf[AARCH64_TIMER_LOG_MAX][AARCH64_TIMER_LOG_LEN];
int  aarch64_timer_log_count;
int  aarch64_timer_log_info_count;
int  aarch64_timer_log_err_count;

static void aarch64_timer_trace_record(enum aarch64_timer_op op, uint64_t value)
{
    if (aarch64_timer_trace_count < AARCH64_TIMER_TRACE_MAX) {
        aarch64_timer_trace[aarch64_timer_trace_count].op = op;
        aarch64_timer_trace[aarch64_timer_trace_count].value = value;
    }
    aarch64_timer_trace_count++;
}

uint64_t cntfrq_el0(void)
{
    aarch64_timer_trace_record(AARCH64_TIMER_OP_FREQ_READ,
                                aarch64_timer_mock_freq);
    return aarch64_timer_mock_freq;
}

void cntp_tval_el0_write(uint64_t v)
{
    aarch64_timer_mock_tval = v;
    aarch64_timer_trace_record(AARCH64_TIMER_OP_TVAL_WRITE, v);
}

uint64_t cntp_ctl_el0_read(void)
{
    uint64_t v = aarch64_timer_mock_ctl;
    if (aarch64_timer_mock_readback_fails)
        v &= ~1UL;   /* the counter refuses to report ENABLE */
    aarch64_timer_trace_record(AARCH64_TIMER_OP_CTL_READ, v);
    return v;
}

void cntp_ctl_el0_write(uint64_t v)
{
    if (aarch64_timer_mock_imask_sticky)
        v |= (1UL << 1);   /* the register refuses to clear IMASK */
    aarch64_timer_mock_ctl = v;
    aarch64_timer_trace_record(AARCH64_TIMER_OP_CTL_WRITE, v);
}

void cntp_isb(void)
{
    aarch64_timer_trace_record(AARCH64_TIMER_OP_ISB, 0);
}

int gic_register_handler(uint32_t intid, gic_handler_fn fn, uint64_t param,
                         const char *name)
{
    (void)fn; (void)param; (void)name;
    aarch64_timer_mock_register_calls++;
    aarch64_timer_mock_register_intid = intid;
    if (aarch64_timer_mock_register_rc != 0)
        return aarch64_timer_mock_register_rc;
    for (int i = 0; i < aarch64_timer_mock_registered_count; i++) {
        if (aarch64_timer_mock_registered[i] == intid)
            return -2;   /* already registered */
    }
    if (aarch64_timer_mock_registered_count < AARCH64_TIMER_MOCK_MAX_INTIDS) {
        aarch64_timer_mock_registered[aarch64_timer_mock_registered_count++] =
            intid;
    }
    return 0;
}

uint32_t dtb_cntp_ppi(void)
{
    return aarch64_timer_mock_ppi;
}

void tick_handler(void)
{
    aarch64_timer_mock_tick_calls++;
}

void aarch64_timer_mock_log(const char *level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (aarch64_timer_log_count < AARCH64_TIMER_LOG_MAX) {
        vsnprintf(aarch64_timer_log_buf[aarch64_timer_log_count],
                  AARCH64_TIMER_LOG_LEN, fmt, ap);
    }
    aarch64_timer_log_count++;
    va_end(ap);
    if (level[0] == 'i')
        aarch64_timer_log_info_count++;
    else if (level[0] == 'e')
        aarch64_timer_log_err_count++;
}

void aarch64_timer_mock_reset(void)
{
    memset(aarch64_timer_mock_registered, 0, sizeof(aarch64_timer_mock_registered));
    aarch64_timer_mock_registered_count = 0;
    aarch64_timer_mock_register_calls = 0;
    aarch64_timer_mock_register_intid = 0;
    aarch64_timer_mock_register_rc = 0;
    aarch64_timer_mock_freq = 62500000ULL;
    aarch64_timer_mock_ctl = 0;
    aarch64_timer_mock_tval = 0;
    aarch64_timer_mock_readback_fails = 0;
    aarch64_timer_mock_imask_sticky = 0;
    aarch64_timer_mock_ppi = 30U;
    aarch64_timer_mock_tick_calls = 0;
    aarch64_timer_trace_count = 0;
    memset(aarch64_timer_trace, 0, sizeof(aarch64_timer_trace));
    aarch64_timer_log_count = 0;
    aarch64_timer_log_info_count = 0;
    aarch64_timer_log_err_count = 0;
    memset(aarch64_timer_log_buf, 0, sizeof(aarch64_timer_log_buf));
}

#endif /* TEST_PLATFORM_H */

#endif /* AARCH64_SCHED_TICK_RUNTIME_H */
