// kernel/include/kernel/subsys.h
#ifndef _KERNEL_SUBSYS_H
#define _KERNEL_SUBSYS_H

#include <stdint.h>

// ── Phase 编号 ────────────────────────────────────────────
// Phase 1-2（CPU 基础设施 / 内存）和 Phase 7-9（TTY / SMP / 调度器）
// 保持硬编码在 kernel_main 中。
#define SUBSYS_PHASE_3   3   // 中断控制器 (APIC/PIC/GIC/PLIC)
#define SUBSYS_PHASE_4   4   // 定时器 (PIT/LAPIC/Generic Timer)
#define SUBSYS_PHASE_5   5   // 设备 IRQ 注册 (键盘、串口 IRQ)
#define SUBSYS_PHASE_6   6   // 存储控制器 (AHCI/VirtIO-BLK)

// ── Flags ─────────────────────────────────────────────────
#define SUBSYS_FLAG_OPTIONAL   (1 << 0)   // init 失败不视为致命

// ── 子系统入口（BSP 一次性 init） ────────────────────────
typedef struct {
    const char *name;           // 子系统名称，用于日志/调试
    int  (*init)(void);         // 初始化函数，0=成功，非0=失败
    int   phase;                // 所属 Phase (3-6)
    uint32_t flags;
    // private:
    int   initialized;          // 0=未执行, 1=成功, <0=失败
} subsys_entry_t;

// ── 子系统入口（per-CPU 二次 init） ──────────────────────
typedef struct {
    const char *name;
    int  (*init_percpu)(int cpu_id);   // 每个在线 CPU 上执行一次
    uint32_t flags;
    // private:
    int initialized;
} subsys_percpu_entry_t;

// ── 函数声明 ─────────────────────────────────────────────
int  register_subsys(const char *name, int (*init)(void),
                     int phase, uint32_t flags);
int  register_subsys_percpu(const char *name,
                            int (*init_percpu)(int cpu_id),
                            uint32_t flags);
void subsys_init_all(void);
void subsys_init_phase(int phase);
void subsys_init_percpu(void);
int  subsys_status(const char *name);

#endif
