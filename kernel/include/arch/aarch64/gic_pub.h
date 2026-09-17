/* kernel/include/arch/aarch64/gic_pub.h — GICv2 跨核发布原语（spec §7.5 R3-1）。
 *
 * 作用：把对全局 `handlers[]` 表的写发布给 Inner-Shareable 域（AP），
 *       保证后续 GICD_SGIR 写触发的 SGI 在 AP 端看到的是已发布过的 handler。
 *
 * 屏障选择：aarch64 `dsb ishst`，与 dmb ishst 的差别在于——
 *   - dmb ishst 仅排序（ordering-only）：保证此屏障前的 store 在屏障后的
 *     store 之前被观察到，但不阻塞屏障后指令的执行。
 *   - dsb ishst 完成 + 指令边界（completion + instruction boundary）：
 *     屏障前的所有 store 必须完成（cache/TLB/write buffer drain 到
 *     Inner-Shareable 域的可见点）才执行屏障后的指令；屏障后指令也
 *     不会重排到屏障前。本 Phase 选 dsb 是有意的较强选择：SGIR 写后
 *     立即发 IRQ，必须保证 handler 表已全局可见。Linux GICv2 选
 *     dmb(ishst) 是因为紧接 dsb sy 做隐含 ordering；OS01 此处直接
 *     dsb ishst 省去再配对屏障的复杂度。
 *
 * 该 wrapper 不进 gic_register_handler()（避免无谓开销），由调用方按需
 * 显式调——Task 3.2 的 gic_ipi_test() 是唯一当前调用点（cntp / pl011_rx
 * / probe_sgi 的注册要么仅 BSP 接收、要么本核自触发，spec §7.5 + §8 R11）。 */
#ifndef _ARCH_AARCH64_GIC_PUB_H
#define _ARCH_AARCH64_GIC_PUB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 发布内核 handler 表给 Inner-Shareable 域。SGIR 写之前必须调。 */
static inline void arch_publish_handler_table(void)
{
    /* dsb ishst = Data Synchronization Barrier, Inner-Shareable,
     * Store-only：屏障前的所有 store 必须完成（对其他核可见）才执行
     * 屏障后的指令；屏障后指令不重排到屏障前。memory clobber 强制
     * 编译器不把对 handlers[] 的写挪到屏障后。 */
    __asm__ __volatile__("dsb ishst" ::: "memory");
}

#ifdef __cplusplus
}
#endif

#endif /* _ARCH_AARCH64_GIC_PUB_H */