/* hosttests/cases/test_boot_failure_log.c — BOOT-3 Task 1.
 *
 * Verifies the cross-architecture boot failure logging boundary:
 *   - Compiles the REAL kernel/core/boot_failure.c.
 *   - Compiles the REAL kernel/arch/aarch64/runtime/log_impl.c.
 *   - Captures log output via kputs/kputu/kputx.
 *   - Simulates arch_local_irq_disable() and arch_cpu_halt() with setjmp/longjmp.
 *   - Asserts INT64_MIN formatting without overflow.
 *   - Asserts %s-only contract (ARM formatter has no %d/%ld).
 *   - Asserts IRQ is disabled BEFORE halt in boot_failure_stop.
 */

#include "test_framework.h"
#include <core/boot_failure.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <stdio.h>

#define INT64_MIN_VAL (-9223372036854775807LL - 1LL)

/* Capture buffer */
static char capture_buf[4096];
static size_t capture_len = 0;

static int irq_disabled = 0;
static int halt_reached = 0;
static int irq_disabled_at_halt = 0;

static jmp_buf halt_jmp;
static int jump_on_halt = 0;

/* Mocks for aarch64 boot_log.h dependencies: kputs, kputu, kputx */
void kputs(const char *s)
{
    if (!s) s = "(null)";
    size_t l = strlen(s);
    if (capture_len + l < sizeof(capture_buf)) {
        memcpy(capture_buf + capture_len, s, l);
        capture_len += l;
        capture_buf[capture_len] = '\0';
    }
}

void kputu(uint64_t v)
{
    char tmp[32];
    int i = 0;
    if (v == 0) {
        kputs("0");
        return;
    }
    while (v > 0) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    char rev[33];
    int r = 0;
    while (i > 0) rev[r++] = tmp[--i];
    rev[r] = '\0';
    kputs(rev);
}

void kputx(uint64_t v)
{
    char tmp[17];
    static const char hex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        tmp[i] = hex[v & 0xF];
        v >>= 4;
    }
    tmp[16] = '\0';
    kputs(tmp);
}

/* Arch mocks */
void arch_local_irq_disable(void)
{
    irq_disabled = 1;
}

void arch_cpu_halt(void)
{
    halt_reached++;
    irq_disabled_at_halt = irq_disabled;
    if (jump_on_halt) {
        longjmp(halt_jmp, 1);
    }
}

static void reset_capture(void)
{
    capture_len = 0;
    capture_buf[0] = '\0';
    irq_disabled = 0;
    halt_reached = 0;
    irq_disabled_at_halt = 0;
    jump_on_halt = 0;
}

static void test_stop_int64_min(void)
{
    reset_capture();
    jump_on_halt = 1;

    if (setjmp(halt_jmp) == 0) {
        boot_failure_stop("subsys", "timer", INT64_MIN_VAL, 4, -1);
    }

    assert_eq(1, halt_reached);
    assert_eq(1, irq_disabled_at_halt);
    assert_str_eq(
        "[boot-3] FATAL stage=subsys component=timer rc=-9223372036854775808 phase=4 cpu=-1\n",
        capture_buf
    );
    assert_null(strstr(capture_buf, "%d"));
    assert_null(strstr(capture_buf, "%ld"));
}

static void test_stop_negative_rc(void)
{
    reset_capture();
    jump_on_halt = 1;

    if (setjmp(halt_jmp) == 0) {
        boot_failure_stop("fs", "root", -5, -1, -1);
    }

    assert_eq(1, halt_reached);
    assert_eq(1, irq_disabled_at_halt);
    assert_str_eq(
        "[boot-3] FATAL stage=fs component=root rc=-5 phase=-1 cpu=-1\n",
        capture_buf
    );
}

static void test_stop_with_cpu(void)
{
    reset_capture();
    jump_on_halt = 1;

    if (setjmp(halt_jmp) == 0) {
        boot_failure_stop("percpu", "lapic", -16, 3, 2);
    }

    assert_eq(1, halt_reached);
    assert_eq(1, irq_disabled_at_halt);
    assert_str_eq(
        "[boot-3] FATAL stage=percpu component=lapic rc=-16 phase=3 cpu=2\n",
        capture_buf
    );
}

static void test_warn_int64_min_and_negative(void)
{
    reset_capture();
    jump_on_halt = 0;

    boot_failure_warn("subsys", "optional-mod", INT64_MIN_VAL, 5, 0);

    assert_eq(0, halt_reached);
    assert_eq(0, irq_disabled);
    assert_str_eq(
        "[boot-3] WARN stage=subsys component=optional-mod rc=-9223372036854775808 phase=5 cpu=0\n",
        capture_buf
    );
    assert_null(strstr(capture_buf, "%d"));
    assert_null(strstr(capture_buf, "%ld"));
}

static void test_null_stage_and_component(void)
{
    reset_capture();
    jump_on_halt = 1;

    if (setjmp(halt_jmp) == 0) {
        boot_failure_stop(NULL, NULL, 0, 0, 0);
    }

    assert_eq(1, halt_reached);
    assert_str_eq(
        "[boot-3] FATAL stage=unknown component=unknown rc=0 phase=0 cpu=0\n",
        capture_buf
    );
}

int main(void)
{
    TEST_SUITE("BOOT-3 boot_failure_log");
    test_stop_int64_min();
    test_stop_negative_rc();
    test_stop_with_cpu();
    test_warn_int64_min_and_negative();
    test_null_stage_and_component();
    TEST_RESULTS();
    return 0;
}
