// kernel/subsys/subsys.c

#include <subsys/subsys.h>
#include <core/boot_failure.h>
#include <log/log.h>
#include <percpu/percpu.h>
#include <errno.h>
#include <string.h>

#define MAX_SUBSYS             64
#define MAX_SUBSYS_CPU_PREPARE 16
#define MAX_SUBSYS_PERCPU      MAX_SUBSYS_CPU_PREPARE

static subsys_entry_t             subsys_table[MAX_SUBSYS];
static int                        subsys_count = 0;
static subsys_cpu_prepare_entry_t subsys_cpu_prepare_table[MAX_SUBSYS_CPU_PREPARE];
static int                        subsys_cpu_prepare_count = 0;

/* Registration latch and sealing state */
static struct {
    int has_error;
    char name[32];
    int rc;
    int phase;
} reg_latch = {0};

static int bsp_sealed = 0;
static int cpu_prepare_sealed = 0;

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
    return register_subsys_deps(name, init, phase, flags, NULL, 0);
}

int register_subsys_deps(const char *name, int (*init)(void),
                         int phase, uint32_t flags,
                         const char *const *requires, unsigned require_count)
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

    if (phase < SUBSYS_PHASE_CONTROLLERS || phase > SUBSYS_PHASE_STORAGE) {
        latch_reg_error(name, -EINVAL, phase);
        return -EINVAL;
    }

    if ((flags & ~SUBSYS_FLAG_OPTIONAL) != 0) {
        latch_reg_error(name, -EINVAL, phase);
        return -EINVAL;
    }

    if (require_count > SUBSYS_MAX_REQUIRES) {
        latch_reg_error(name, -EINVAL, phase);
        return -EINVAL;
    }

    if (require_count > 0 && !requires) {
        latch_reg_error(name, -EINVAL, phase);
        return -EINVAL;
    }

    for (unsigned k = 0; k < require_count; k++) {
        if (!requires[k] || requires[k][0] == '\0') {
            latch_reg_error(name, -EINVAL, phase);
            return -EINVAL;
        }
        if (strcmp(requires[k], name) == 0) {
            latch_reg_error(name, -EINVAL, phase);
            return -EINVAL;
        }
        for (unsigned j = 0; j < k; j++) {
            if (strcmp(requires[k], requires[j]) == 0) {
                latch_reg_error(name, -EINVAL, phase);
                return -EINVAL;
            }
        }
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
    e->name          = name;
    e->init          = init;
    e->phase         = phase;
    e->flags         = flags;
    e->requires      = requires;
    e->require_count = require_count;
    e->initialized   = 0;
    e->raw_result    = -EAGAIN;
    subsys_count++;
    return 0;
}

// ── 注册（BSP CPU 数据准备） ──────────────────────────────

int register_subsys_cpu_prepare(const char *name,
                                 int (*prepare_for_cpu)(int target_cpu),
                                 uint32_t flags)
{
    if (reg_latch.has_error)
        return -EINVAL;

    if (cpu_prepare_sealed) {
        latch_reg_error(name ? name : "late_register", -EINVAL, -1);
        return -EINVAL;
    }

    if (!name || name[0] == '\0' || !prepare_for_cpu) {
        latch_reg_error(name, -EINVAL, -1);
        return -EINVAL;
    }

    if ((flags & ~SUBSYS_FLAG_OPTIONAL) != 0) {
        latch_reg_error(name, -EINVAL, -1);
        return -EINVAL;
    }

    for (int i = 0; i < subsys_cpu_prepare_count; i++) {
        if (strcmp(subsys_cpu_prepare_table[i].name, name) == 0) {
            latch_reg_error(name, -EEXIST, -1);
            return -EEXIST;
        }
    }

    if (subsys_cpu_prepare_count >= MAX_SUBSYS_CPU_PREPARE) {
        latch_reg_error(name, -ENOSPC, -1);
        return -ENOSPC;
    }

    subsys_cpu_prepare_entry_t *e = &subsys_cpu_prepare_table[subsys_cpu_prepare_count];
    e->name            = name;
    e->prepare_for_cpu = prepare_for_cpu;
    e->flags           = flags;
    e->initialized     = 0;
    e->raw_result      = 0;
    e->failed_mask     = 0;
    subsys_cpu_prepare_count++;
    return 0;
}

int register_subsys_percpu(const char *name,
                            int (*init_percpu)(int cpu_id),
                            uint32_t flags)
{
    return register_subsys_cpu_prepare(name, init_percpu, flags);
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

// ── Preflight 依赖与拓扑校验 ─────────────────────────────

static int find_bsp_subsys(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < subsys_count; i++) {
        if (strcmp(subsys_table[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void preflight_bsp_table(void)
{
    /* 1. 依赖存在性及未来阶段检查（按注册序） */
    for (int i = 0; i < subsys_count; i++) {
        subsys_entry_t *cons = &subsys_table[i];
        for (unsigned k = 0; k < cons->require_count; k++) {
            const char *req = cons->requires[k];
            int p_idx = find_bsp_subsys(req);
            if (p_idx < 0) {
                latch_reg_error(cons->name, -ENOENT, cons->phase);
                boot_failure_stop("register", cons->name, -ENOENT, cons->phase, -1);
            }
            if (subsys_table[p_idx].phase > cons->phase) {
                latch_reg_error(cons->name, -EINVAL, cons->phase);
                boot_failure_stop("register", cons->name, -EINVAL, cons->phase, -1);
            }
        }
    }

    /* 2. 有界图环检测：
     * 为每个节点按注册序检查从自身出发能否回到自身。
     * 首个满足者即为最早实际环成员（诊断报告最早实际环成员而非其消费者）。
     */
    for (int i = 0; i < subsys_count; i++) {
        int queue[MAX_SUBSYS];
        int head = 0, tail = 0;
        uint64_t visited = 0;

        for (unsigned k = 0; k < subsys_table[i].require_count; k++) {
            int dep = find_bsp_subsys(subsys_table[i].requires[k]);
            if (dep >= 0) {
                if (dep == i) {
                    latch_reg_error(subsys_table[i].name, -ELOOP, subsys_table[i].phase);
                    boot_failure_stop("register", subsys_table[i].name, -ELOOP, subsys_table[i].phase, -1);
                }
                queue[tail++] = dep;
            }
        }

        while (head < tail) {
            int curr = queue[head++];
            if (!(visited & (1ULL << curr))) {
                visited |= (1ULL << curr);
                for (unsigned k = 0; k < subsys_table[curr].require_count; k++) {
                    int dep = find_bsp_subsys(subsys_table[curr].requires[k]);
                    if (dep >= 0) {
                        if (dep == i) {
                            latch_reg_error(subsys_table[i].name, -ELOOP, subsys_table[i].phase);
                            boot_failure_stop("register", subsys_table[i].name, -ELOOP, subsys_table[i].phase, -1);
                        }
                        if (!(visited & (1ULL << dep))) {
                            queue[tail++] = dep;
                        }
                    }
                }
            }
        }
    }
}

// ── 执行（BSP 一次性 init，按 phase） ─────────────────────

void subsys_init_phase(int phase)
{
    if (phase < SUBSYS_PHASE_CONTROLLERS || phase > SUBSYS_PHASE_STORAGE) {
        boot_failure_stop("subsys", "phase", -EINVAL, phase, -1);
    }

    if (reg_latch.has_error) {
        boot_failure_stop("register", reg_latch.name, reg_latch.rc,
                          reg_latch.phase, -1);
    }

    if (!bsp_sealed) {
        bsp_sealed = 1;
        preflight_bsp_table();
    }

    for (int round = 0; round < MAX_SUBSYS; round++) {
        int uninit_in_phase = 0;
        int progress = 0;

        for (int i = 0; i < subsys_count; i++) {
            subsys_entry_t *e = &subsys_table[i];
            if (e->phase != phase || e->initialized != 0)
                continue;

            uninit_in_phase++;

            int can_run = 1;
            for (unsigned k = 0; k < e->require_count; k++) {
                int p_idx = find_bsp_subsys(e->requires[k]);
                subsys_entry_t *prod = &subsys_table[p_idx];

                if (prod->phase < phase) {
                    if (prod->initialized == 0) {
                        log_err("[boot-4] blocked component=%s dependency=%s\n", e->name, prod->name);
                        boot_failure_stop("subsys", e->name, -EAGAIN, phase, -1);
                    }
                    if (prod->initialized != 1) {
                        log_err("[boot-4] blocked component=%s dependency=%s\n", e->name, prod->name);
                        if (!(e->flags & SUBSYS_FLAG_OPTIONAL)) {
                            boot_failure_stop("subsys", e->name, -ENODEV, phase, -1);
                        } else {
                            e->initialized = -1;
                            e->raw_result = -ENODEV;
                            boot_failure_warn("subsys", e->name, -ENODEV, phase, -1);
                            can_run = 0;
                            progress = 1;
                            break;
                        }
                    }
                } else {
                    /* prod->phase == phase */
                    if (prod->initialized == 0) {
                        can_run = 0;
                        break;
                    }
                    if (prod->initialized != 1) {
                        log_err("[boot-4] blocked component=%s dependency=%s\n", e->name, prod->name);
                        if (!(e->flags & SUBSYS_FLAG_OPTIONAL)) {
                            boot_failure_stop("subsys", e->name, -ENODEV, phase, -1);
                        } else {
                            e->initialized = -1;
                            e->raw_result = -ENODEV;
                            boot_failure_warn("subsys", e->name, -ENODEV, phase, -1);
                            can_run = 0;
                            progress = 1;
                            break;
                        }
                    }
                }
            }

            if (can_run) {
                int ret = e->init();
                e->raw_result = ret;
                e->initialized = (ret == 0) ? 1 : -1;
                progress = 1;

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

        if (uninit_in_phase == 0)
            break;

        if (!progress) {
            for (int i = 0; i < subsys_count; i++) {
                if (subsys_table[i].phase == phase && subsys_table[i].initialized == 0) {
                    boot_failure_stop("register", subsys_table[i].name, -ELOOP, phase, -1);
                }
            }
            break;
        }
    }
}

void subsys_init_all(void)
{
    for (int phase = SUBSYS_PHASE_CONTROLLERS; phase <= SUBSYS_PHASE_STORAGE; phase++)
        subsys_init_phase(phase);
}

// ── 执行（BSP CPU 数据准备） ──────────────────────────────

void subsys_prepare_cpus(void)
{
    if (reg_latch.has_error) {
        boot_failure_stop("register", reg_latch.name, reg_latch.rc,
                          reg_latch.phase, -1);
    }

    cpu_prepare_sealed = 1;

    for (int i = 0; i < subsys_cpu_prepare_count; i++) {
        subsys_cpu_prepare_entry_t *e = &subsys_cpu_prepare_table[i];
        if (e->initialized != 0)
            continue;

        int first_fail_rc = 0;
        uint64_t fail_mask = 0;

        for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
            int ret = e->prepare_for_cpu((int)cpu);

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
                log_warn("subsys: cpu-prepare %s target=%u executor=bsp ... SKIP (optional, ret=%d)\n",
                         e->name, cpu, ret);
            } else {
                log_info("subsys: cpu-prepare %s target=%u executor=bsp ... ok\n",
                         e->name, cpu);
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

void subsys_init_percpu(void)
{
    subsys_prepare_cpus();
}

// ── 查询 ─────────────────────────────────────────────────

int subsys_status(const char *name)
{
    if (!name) return -999;
    for (int i = 0; i < subsys_count; i++)
        if (strcmp(subsys_table[i].name, name) == 0)
            return subsys_table[i].initialized;
    for (int i = 0; i < subsys_cpu_prepare_count; i++)
        if (strcmp(subsys_cpu_prepare_table[i].name, name) == 0)
            return subsys_cpu_prepare_table[i].initialized;
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

int subsys_cpu_prepare_result(const char *name, uint64_t *failed_mask)
{
    if (!name) {
        if (failed_mask) *failed_mask = 0;
        return -ENOENT;
    }
    for (int i = 0; i < subsys_cpu_prepare_count; i++) {
        if (strcmp(subsys_cpu_prepare_table[i].name, name) == 0) {
            if (failed_mask)
                *failed_mask = subsys_cpu_prepare_table[i].failed_mask;
            return subsys_cpu_prepare_table[i].raw_result;
        }
    }
    if (failed_mask) *failed_mask = 0;
    return -ENOENT;
}

int subsys_percpu_result(const char *name, uint64_t *failed_mask)
{
    return subsys_cpu_prepare_result(name, failed_mask);
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
    subsys_cpu_prepare_count = 0;
    memset(subsys_table, 0, sizeof(subsys_table));
    memset(subsys_cpu_prepare_table, 0, sizeof(subsys_cpu_prepare_table));
    memset(&reg_latch, 0, sizeof(reg_latch));
    bsp_sealed = 0;
    cpu_prepare_sealed = 0;
}
#endif
