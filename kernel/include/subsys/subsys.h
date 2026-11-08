// kernel/include/subsys/subsys.h
#ifndef _KERNEL_SUBSYS_H
#define _KERNEL_SUBSYS_H

#include <stdint.h>

// ── Phase 编号 ────────────────────────────────────────────
// Phase 1-2（CPU 基础设施 / 内存）和 Phase 7-9（TTY / SMP / 调度器）
// 保持硬编码在 kernel_main 中。
#define SUBSYS_PHASE_CONTROLLERS 3   // 中断控制器 (APIC/PIC/GIC/PLIC)
#define SUBSYS_PHASE_TIME        4   // 定时器 (PIT/LAPIC/Generic Timer)
#define SUBSYS_PHASE_DEVICE_IRQ  5   // 设备 IRQ 注册 (键盘、串口 IRQ)
#define SUBSYS_PHASE_STORAGE     6   // 存储控制器 (AHCI/VirtIO-BLK)

// 兼容别名（值相同）
#define SUBSYS_PHASE_3   SUBSYS_PHASE_CONTROLLERS
#define SUBSYS_PHASE_4   SUBSYS_PHASE_TIME
#define SUBSYS_PHASE_5   SUBSYS_PHASE_DEVICE_IRQ
#define SUBSYS_PHASE_6   SUBSYS_PHASE_STORAGE

// ── Flags & Limits ────────────────────────────────────────
#define SUBSYS_FLAG_OPTIONAL   (1 << 0)   // init 失败不视为致命
#define SUBSYS_MAX_REQUIRES    8          // 每项最多依赖数量

// ── 子系统入口（BSP 一次性 init） ────────────────────────
typedef struct {
    const char *name;           // 子系统名称，用于日志/调试
    int  (*init)(void);         // 初始化函数，0=成功，非0=失败
    int   phase;                // 所属 Phase (3-6)
    uint32_t flags;
    const char *const *requires;// 依赖项名称数组（静态生命周期）
    unsigned require_count;     // 依赖项数量 (0..SUBSYS_MAX_REQUIRES)
    // private:
    int   initialized;          // 0=未执行, 1=成功, <0=失败
    int   raw_result;           // 原始返回码
} subsys_entry_t;

// ── 子系统入口（per-CPU 二次 init） ──────────────────────
typedef struct {
    const char *name;
    int  (*init_percpu)(int cpu_id);   // 每个在线 CPU 上执行一次
    uint32_t flags;
    // private:
    int initialized;
    int raw_result;
    uint64_t failed_mask;
} subsys_percpu_entry_t;

typedef int (*subsys_initcall_t)(void);

// ── 函数声明 ─────────────────────────────────────────────
int  register_subsys(const char *name, int (*init)(void),
                     int phase, uint32_t flags);
int  register_subsys_deps(const char *name, int (*init)(void),
                         int phase, uint32_t flags,
                         const char *const *requires, unsigned require_count);
int  register_subsys_percpu(const char *name,
                            int (*init_percpu)(int cpu_id),
                            uint32_t flags);
void subsys_init_all(void);
void subsys_init_phase(int phase);
void subsys_init_percpu(void);
int  subsys_status(const char *name);
void subsys_register_initcalls(subsys_initcall_t *begin, subsys_initcall_t *end);
int  subsys_result(const char *name);
int  subsys_percpu_result(const char *name, uint64_t *failed_mask);
int  subsys_require_ready(const char *name);

#if defined(OS01_HOST_TEST)
void subsys_test_reset(void);
#endif

// ── Self-registration via linker section ─────────────────
// Drivers place a function pointer in the .subsys_init section via
// SUBSYS_INITCALL(). The platform's arch_register_subsys() iterates
// __subsys_init_start..__subsys_init_end and calls each. This avoids
// __attribute__((constructor)), which has no runtime support in this
// libc-free kernel (no .init_array iteration in startup code).

#define SUBSYS_INITCALL(fn)                                            \
    static subsys_initcall_t __subsys_initcall_##fn                   \
        __attribute__((used, section(".subsys_init"))) = fn

extern subsys_initcall_t __subsys_init_start[];
extern subsys_initcall_t __subsys_init_end[];

#endif
