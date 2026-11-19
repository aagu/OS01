#ifndef ARCH_SMP_H
#define ARCH_SMP_H

#include <stdint.h>

struct percpu;

/* 统一 AP C 入口签名（双架构汇编入口均按各自 ABI 传入 cpu_id 与 struct percpu *） */
typedef void (*arch_smp_ap_entry_t)(uint32_t cpu_id, struct percpu *cpu)
    __attribute__((noreturn));

/* 统一 SMP 启动失败阶段分类 */
enum smp_boot_stage {
    SMP_STAGE_INVARIANT = 0, /* 前置或后置状态不变量违背 */
    SMP_STAGE_PREPARE,       /* AP 资源准备失败（如 idle 栈/metadata/发布失败） */
    SMP_STAGE_ACTIVATE,      /* 底层唤醒失败（如 PSCI 不可用或 CPU_ON 拒绝） */
    SMP_STAGE_ONLINE,        /* AP 未在有界预算内发布 online / boot ACK */
    SMP_STAGE_SCHEDULER,     /* AP 未在有界预算内发布 scheduler_ok（x86_64） */
};

/* 最近一次 arch_smp_boot_aps() 的只读诊断快照 */
struct arch_smp_boot_status {
    uint32_t requested;              /* 请求的总 CPU 数（含 BSP） */
    uint32_t online;                 /* 确认上线的 CPU 数（含 BSP） */
    uint64_t online_mask;            /* bit i = 1 表示逻辑 CPU i 已确认上线 */
    uint32_t failed_cpu;             /* 失败的逻辑 CPU ID（成功时为 0） */
    enum smp_boot_stage failed_stage;/* 失败阶段 */
    int32_t  arch_rc;                /* 架构底层错误码（如 PSCI rc 或 -ETIMEDOUT） */
};

/*
 * 单入口 SMP AP 启动（强覆盖，双架构各自实现）：
 * - cpu_count:    逻辑 CPU 总数（含 BSP，必须满足 1 <= cpu_count <= NR_CPUS）
 * - entry:        非空统一 AP C 入口（由公共层传入 smp_ap_common_entry）
 * - per_cpu_data: 指向全局 percpu_data[0..cpu_count-1]
 *
 * 返回值：
 * - 0：所有 AP（1..cpu_count-1）均成功完成本架构阶段 8 握手；
 * - 负 errno（-EINVAL / -ENOMEM / -ENODEV / -EIO / -ETIMEDOUT）：
 *   任一 AP 失败，且 arch_smp_last_status() 已记录失败核、阶段与底层错误码。
 */
int arch_smp_boot_aps(uint32_t cpu_count,
                      arch_smp_ap_entry_t entry,
                      struct percpu *per_cpu_data);

/* 只读查询最近一次 arch_smp_boot_aps() 的状态快照（强覆盖） */
const struct arch_smp_boot_status *arch_smp_last_status(void);

/* 公共 AP C 入口状态机（由 kernel/core/smp.c 实现，暴露原型以供宿主单测直接驱动） */
void smp_ap_common_entry(uint32_t cpu_id, struct percpu *cpu)
    __attribute__((noreturn));

/*
 * AP 生命周期底层钩子（强覆盖，仅由 kernel/core/smp.c::smp_ap_common_entry 调用）：
 * 1. arch_smp_ap_cpu_init:          本核硬件初始化，必须在 online=1 发布前完成
 * 2. arch_smp_ap_post_online:       online=1 发布后、开中断前的架构握手
 * 3. arch_smp_ap_publish_ipi_ready: 开中断并执行屏障后发布 ipi_ready=1
 * 4. arch_smp_ap_enter_idle:        进入 idle / 调度交接循环（永不返回）
 */
void arch_smp_ap_cpu_init(uint32_t cpu_id, struct percpu *cpu);
void arch_smp_ap_post_online(uint32_t cpu_id, struct percpu *cpu);
void arch_smp_ap_publish_ipi_ready(uint32_t cpu_id, struct percpu *cpu);
void arch_smp_ap_enter_idle(uint32_t cpu_id, struct percpu *cpu)
    __attribute__((noreturn));

/*
 * 可选的 BSP 启动后钩子（kernel/core/smp.c 提供弱默认 no-op）：
 * 仅在所有 AP 启动成功后、subsys_prepare_cpus() 之前执行。
 * aarch64 强覆盖用于执行 test_spinlock_smp(online_cpus)。
 */
void arch_smp_post_boot_hook(uint32_t online_cpus);

#endif /* ARCH_SMP_H */
