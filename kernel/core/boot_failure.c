#include <core/boot_failure.h>
#include <log/log.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <stdint.h>
#include <stddef.h>

static void format_i64(int64_t val, char *buf, size_t size)
{
    if (size == 0) return;
    char tmp[32];
    size_t i = 0;
    uint64_t uval;
    int neg = 0;
    if (val < 0) {
        neg = 1;
        uval = (uint64_t)(-(val + 1)) + 1;
    } else {
        uval = (uint64_t)val;
    }
    if (uval == 0) {
        tmp[i++] = '0';
    } else {
        while (uval > 0 && i < sizeof(tmp)) {
            tmp[i++] = (char)('0' + (uval % 10));
            uval /= 10;
        }
    }
    size_t out = 0;
    if (neg && out + 1 < size) {
        buf[out++] = '-';
    }
    while (i > 0 && out + 1 < size) {
        buf[out++] = tmp[--i];
    }
    buf[out] = '\0';
}

__attribute__((noreturn))
void boot_failure_stop(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu)
{
    arch_local_irq_disable();
    char rc_buf[32];
    char phase_buf[32];
    char cpu_buf[32];
    format_i64(rc, rc_buf, sizeof(rc_buf));
    format_i64(phase, phase_buf, sizeof(phase_buf));
    format_i64(cpu, cpu_buf, sizeof(cpu_buf));
    log_err("[boot-3] FATAL stage=%s component=%s rc=%s phase=%s cpu=%s\n",
            stage ? stage : "unknown", component ? component : "unknown",
            rc_buf, phase_buf, cpu_buf);
    for (;;) {
        arch_cpu_halt();
    }
}

void boot_failure_warn(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu)
{
    char rc_buf[32];
    char phase_buf[32];
    char cpu_buf[32];
    format_i64(rc, rc_buf, sizeof(rc_buf));
    format_i64(phase, phase_buf, sizeof(phase_buf));
    format_i64(cpu, cpu_buf, sizeof(cpu_buf));
    log_warn("[boot-3] WARN stage=%s component=%s rc=%s phase=%s cpu=%s\n",
             stage ? stage : "unknown", component ? component : "unknown",
             rc_buf, phase_buf, cpu_buf);
}
