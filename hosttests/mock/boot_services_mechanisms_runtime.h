#ifndef BOOT_SERVICES_MECHANISMS_RUNTIME_H
#define BOOT_SERVICES_MECHANISMS_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <arch/x86_64/tss.h>

/* Trackers for mechanisms */
typedef struct {
    int call_count;
    uint32_t *table;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
} set_tss64_record_t;

extern set_tss64_record_t g_set_tss64_rec;
extern int g_arch_cpu_halt_calls;
extern int g_schedule_calls;
extern int g_arch_local_irq_enable_calls;
extern bool g_irq_enabled;
extern jmp_buf g_mechanisms_exit_jmp;

#endif /* BOOT_SERVICES_MECHANISMS_RUNTIME_H */
