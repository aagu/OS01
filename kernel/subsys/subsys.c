// kernel/subsys/subsys.c

#include <kernel/subsys.h>
#include <kernel/printk.h>
#include <kernel/percpu.h>
#include <string.h>

#define MAX_SUBSYS        64
#define MAX_SUBSYS_PERCPU 16

static subsys_entry_t        subsys_table[MAX_SUBSYS];
static int                   subsys_count = 0;
static subsys_percpu_entry_t subsys_percpu_table[MAX_SUBSYS_PERCPU];
static int                   subsys_percpu_count = 0;

// ── 注册（BSP 一次性 init） ───────────────────────────────

int register_subsys(const char *name, int (*init)(void),
                    int phase, uint32_t flags)
{
    if (!name || !init || subsys_count >= MAX_SUBSYS)
        return -1;
    subsys_entry_t *e = &subsys_table[subsys_count];
    e->name   = name;
    e->init   = init;
    e->phase  = phase;
    e->flags  = flags;
    e->initialized = 0;
    subsys_count++;
    return 0;
}

// ── 注册（per-CPU 二次 init） ─────────────────────────────

int register_subsys_percpu(const char *name,
                           int (*init_percpu)(int cpu_id),
                           uint32_t flags)
{
    if (!name || !init_percpu || subsys_percpu_count >= MAX_SUBSYS_PERCPU)
        return -1;
    subsys_percpu_entry_t *e = &subsys_percpu_table[subsys_percpu_count];
    e->name        = name;
    e->init_percpu = init_percpu;
    e->flags       = flags;
    e->initialized = 0;
    subsys_percpu_count++;
    return 0;
}

// ── 执行（BSP 一次性 init，按 phase） ─────────────────────

void subsys_init_phase(int phase)
{
    for (int i = 0; i < subsys_count; i++) {
        subsys_entry_t *e = &subsys_table[i];
        if (e->phase != phase || e->initialized != 0)
            continue;

        serial_printk("subsys: init  %s ... ", e->name);
        int ret = e->init();
        e->initialized = (ret == 0) ? 1 : -1;

        if (ret == 0) {
            serial_printk("ok\n");
        } else if (e->flags & SUBSYS_FLAG_OPTIONAL) {
            serial_printk("SKIP (optional, ret=%d)\n", ret);
        } else {
            serial_printk("FAIL (ret=%d)\n", ret);
        }
    }
}

void subsys_init_all(void)
{
    for (int phase = 3; phase <= 6; phase++)
        subsys_init_phase(phase);
}

// ── 执行（per-CPU 二次 init） ─────────────────────────────

void subsys_init_percpu(void)
{
    for (int i = 0; i < subsys_percpu_count; i++) {
        subsys_percpu_entry_t *e = &subsys_percpu_table[i];
        if (e->initialized)
            continue;

        for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
            serial_printk("subsys: percpu %s cpu=%u ... ", e->name, cpu);
            int ret = e->init_percpu((int)cpu);
            if (ret != 0 && !(e->flags & SUBSYS_FLAG_OPTIONAL)) {
                serial_printk("FAIL (ret=%d)\n", ret);
            } else {
                serial_printk("ok\n");
            }
        }
        e->initialized = 1;
    }
}

// ── 查询 ─────────────────────────────────────────────────

int subsys_status(const char *name)
{
    for (int i = 0; i < subsys_count; i++)
        if (strcmp(subsys_table[i].name, name) == 0)
            return subsys_table[i].initialized;
    return -999;
}
