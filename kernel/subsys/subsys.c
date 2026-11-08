// kernel/subsys/subsys.c

#include <subsys/subsys.h>
#include <core/boot_failure.h>
#include <log/log.h>
#include <percpu/percpu.h>
#include <errno.h>
#include <string.h>

#define MAX_SUBSYS        64
#define MAX_SUBSYS_PERCPU 16

static subsys_entry_t        subsys_table[MAX_SUBSYS];
static int                   subsys_count = 0;
static subsys_percpu_entry_t subsys_percpu_table[MAX_SUBSYS_PERCPU];
static int                   subsys_percpu_count = 0;

/* Registration latch and sealing state */
static struct {
    int has_error;
    char name[32];
    int rc;
    int phase;
} reg_latch = {0};

static int bsp_sealed = 0;
static int percpu_sealed = 0;

static void latch_reg_error(const char *name, int rc, int phase)
{
    if (reg_latch.has_error)
        return;
    reg_latch.has_error = 1;
    reg_latch.rc = rc;
    reg_latch.phase = phase;
    if (name && name[0] != '\0') {
        size_t len = strlen(name);
        if (len >= sizeof(reg_latch.name))
            len = sizeof(reg_latch.name) - 1;
        memcpy(reg_latch.name, name, len);
        reg_latch.name[len] = '\0';
    } else {
        memcpy(reg_latch.name, "unknown", 8);
    }
}

// ── 注册（BSP 一次性 init） ───────────────────────────────

int register_subsys(const char *name, int (*init)(void),
                    int phase, uint32_t flags)
{
    if (reg_latch.has_error)
        return -EINVAL;

    if (bsp_sealed) {
        latch_reg_error(name ? name : "late_register", -EINVAL, phase);
        return -EINVAL;
    }

    if (!name || name[0] == '\0' || !init) {
        latch_reg_error(name, -EINVAL, phase);
        return -EINVAL;
    }

    if (phase < SUBSYS_PHASE_3 || phase > SUBSYS_PHASE_6) {
        latch_reg_error(name, -EINVAL, phase);
        return -EINVAL;
    }

    if ((flags & ~SUBSYS_FLAG_OPTIONAL) != 0) {
        latch_reg_error(name, -EINVAL, phase);
        return -EINVAL;
    }

    for (int i = 0; i < subsys_count; i++) {
        if (strcmp(subsys_table[i].name, name) == 0) {
            latch_reg_error(name, -EEXIST, phase);
            return -EEXIST;
        }
    }

    if (subsys_count >= MAX_SUBSYS) {
        latch_reg_error(name, -ENOSPC, phase);
        return -ENOSPC;
    }

    subsys_entry_t *e = &subsys_table[subsys_count];
    e->name   = name;
    e->init   = init;
    e->phase  = phase;
    e->flags  = flags;
    e->initialized = 0;
    e->raw_result  = -EAGAIN;
    subsys_count++;
    return 0;
}

// ── 注册（per-CPU 二次 init） ─────────────────────────────

int register_subsys_percpu(const char *name,
                            int (*init_percpu)(int cpu_id),
                            uint32_t flags)
{
    if (reg_latch.has_error)
        return -EINVAL;

    if (percpu_sealed) {
        latch_reg_error(name ? name : "late_register", -EINVAL, -1);
        return -EINVAL;
    }

    if (!name || name[0] == '\0' || !init_percpu) {
        latch_reg_error(name, -EINVAL, -1);
        return -EINVAL;
    }

    if ((flags & ~SUBSYS_FLAG_OPTIONAL) != 0) {
        latch_reg_error(name, -EINVAL, -1);
        return -EINVAL;
    }

    for (int i = 0; i < subsys_percpu_count; i++) {
        if (strcmp(subsys_percpu_table[i].name, name) == 0) {
            latch_reg_error(name, -EEXIST, -1);
            return -EEXIST;
        }
    }

    if (subsys_percpu_count >= MAX_SUBSYS_PERCPU) {
        latch_reg_error(name, -ENOSPC, -1);
        return -ENOSPC;
    }

    subsys_percpu_entry_t *e = &subsys_percpu_table[subsys_percpu_count];
    e->name        = name;
    e->init_percpu = init_percpu;
    e->flags       = flags;
    e->initialized = 0;
    e->raw_result  = -EAGAIN;
    e->failed_mask = 0;
    subsys_percpu_count++;
    return 0;
}

// ── Common initcall iterator ──────────────────────────────

void subsys_register_initcalls(subsys_initcall_t *begin, subsys_initcall_t *end)
{
    for (subsys_initcall_t *p = begin; p < end; p++) {
        if (reg_latch.has_error) {
            boot_failure_stop("register", reg_latch.name, reg_latch.rc,
                              reg_latch.phase, -1);
        }
        if (!p || !*p) {
            boot_failure_stop("register", "initcall", -EINVAL, -1, -1);
        }
        int ret = (*p)();
        if (reg_latch.has_error) {
            boot_failure_stop("register", reg_latch.name, reg_latch.rc,
                              reg_latch.phase, -1);
        }
        if (ret != 0) {
            boot_failure_stop("register", "initcall", ret, -1, -1);
        }
    }
}

// ── 执行（BSP 一次性 init，按 phase） ─────────────────────

void subsys_init_phase(int phase)
{
    if (reg_latch.has_error) {
        boot_failure_stop("register", reg_latch.name, reg_latch.rc,
                          reg_latch.phase, -1);
    }

    bsp_sealed = 1;

    for (int i = 0; i < subsys_count; i++) {
        subsys_entry_t *e = &subsys_table[i];
        if (e->phase != phase || e->initialized != 0)
            continue;

        int ret = e->init();
        e->raw_result = ret;
        e->initialized = (ret == 0) ? 1 : -1;

        if (reg_latch.has_error) {
            boot_failure_stop("register", reg_latch.name, reg_latch.rc,
                              reg_latch.phase, -1);
        }

        if (ret == 0) {
            log_info("subsys: init  %s ... ok\n", e->name);
        } else if (e->flags & SUBSYS_FLAG_OPTIONAL) {
            boot_failure_warn("subsys", e->name, ret, phase, -1);
            log_warn("subsys: init  %s ... SKIP (optional, ret=%d)\n", e->name, ret);
        } else {
            boot_failure_stop("subsys", e->name, ret, phase, -1);
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
    if (reg_latch.has_error) {
        boot_failure_stop("register", reg_latch.name, reg_latch.rc,
                          reg_latch.phase, -1);
    }

    percpu_sealed = 1;

    for (int i = 0; i < subsys_percpu_count; i++) {
        subsys_percpu_entry_t *e = &subsys_percpu_table[i];
        if (e->initialized != 0)
            continue;

        int first_fail_rc = 0;
        uint64_t fail_mask = 0;

        for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
            int ret = e->init_percpu((int)cpu);

            if (reg_latch.has_error) {
                boot_failure_stop("register", reg_latch.name, reg_latch.rc,
                                  reg_latch.phase, (int)cpu);
            }

            if (ret != 0 && !(e->flags & SUBSYS_FLAG_OPTIONAL)) {
                boot_failure_stop("percpu", e->name, ret, -1, (int)cpu);
            } else if (ret != 0) {
                if (first_fail_rc == 0)
                    first_fail_rc = ret;
                fail_mask |= (1ULL << cpu);
                boot_failure_warn("percpu", e->name, ret, -1, (int)cpu);
                log_warn("subsys: percpu %s cpu=%u ... SKIP (optional, ret=%d)\n", e->name, cpu, ret);
            } else {
                log_info("subsys: percpu %s cpu=%u ... ok\n", e->name, cpu);
            }
        }

        e->failed_mask = fail_mask;
        if (fail_mask != 0) {
            e->raw_result = first_fail_rc;
            e->initialized = -1;
        } else {
            e->raw_result = 0;
            e->initialized = 1;
        }
    }
}

// ── 查询 ─────────────────────────────────────────────────

int subsys_status(const char *name)
{
    if (!name) return -999;
    for (int i = 0; i < subsys_count; i++)
        if (strcmp(subsys_table[i].name, name) == 0)
            return subsys_table[i].initialized;
    for (int i = 0; i < subsys_percpu_count; i++)
        if (strcmp(subsys_percpu_table[i].name, name) == 0)
            return subsys_percpu_table[i].initialized;
    return -999;
}

int subsys_result(const char *name)
{
    if (!name) return -ENOENT;
    for (int i = 0; i < subsys_count; i++)
        if (strcmp(subsys_table[i].name, name) == 0)
            return subsys_table[i].raw_result;
    return -ENOENT;
}

int subsys_percpu_result(const char *name, uint64_t *failed_mask)
{
    if (!name) {
        if (failed_mask) *failed_mask = 0;
        return -ENOENT;
    }
    for (int i = 0; i < subsys_percpu_count; i++) {
        if (strcmp(subsys_percpu_table[i].name, name) == 0) {
            if (failed_mask)
                *failed_mask = subsys_percpu_table[i].failed_mask;
            return subsys_percpu_table[i].raw_result;
        }
    }
    if (failed_mask) *failed_mask = 0;
    return -ENOENT;
}

int subsys_require_ready(const char *name)
{
    if (!name) return -ENOENT;
    for (int i = 0; i < subsys_count; i++) {
        if (strcmp(subsys_table[i].name, name) == 0) {
            if (subsys_table[i].initialized == 0)
                return -EAGAIN;
            if (subsys_table[i].initialized == 1)
                return 0;
            int raw = subsys_table[i].raw_result;
            if (raw > 0)
                return -EIO;
            return (raw != 0) ? raw : -EIO;
        }
    }
    return -ENOENT;
}

#if defined(OS01_HOST_TEST)
void subsys_test_reset(void)
{
    subsys_count = 0;
    subsys_percpu_count = 0;
    memset(subsys_table, 0, sizeof(subsys_table));
    memset(subsys_percpu_table, 0, sizeof(subsys_percpu_table));
    memset(&reg_latch, 0, sizeof(reg_latch));
    bsp_sealed = 0;
    percpu_sealed = 0;
}
#endif
