#ifndef _ARCH_RANDOM_H
#define _ARCH_RANDOM_H

#include <stdint.h>
#include <stdbool.h>

/* OS01 arch-neutral entropy facade. 语义定义见 docs/arch/entropy-source-facade.md §2。
 * 本 header 是 STRONG/WEAK/NONE 定义的唯一来源（spec §0.2.1）；架构 .c 与调用方不重述。 */
typedef enum {
    ARCH_ENTROPY_NONE  = 0,
    ARCH_ENTROPY_WEAK  = 1,
    ARCH_ENTROPY_STRONG = 2,
} arch_entropy_source_t;

/* 一次性产出 32B entropy + 质量标签（spec §3）。
 *
 *   out[32]   成功（*quality != NONE）时填满 32B；失败时 memset(out, 0, 32)。
 *   *quality  成功时设 STRONG/WEAK；失败时设 NONE。
 *   return    true iff *quality != NONE。
 *
 * IRQ 安全。Boot 前可调（架构实现各自测一次）。
 */
bool arch_random_get_entropy(uint8_t out[32], arch_entropy_source_t *quality);

/* AT_RANDOM 专用 helper：拒绝 WEAK/NONE（spec §6）。
 *
 *   out[32]   成功时填满 32B（STRONG）。
 *   return    true iff STRONG 已填入。
 *
 * 失败时 out 内容调用方契约不依赖 — caller 须把 out 当作不可用。
 */
bool arch_random_get_strong(uint8_t out[32]);

#endif // _ARCH_RANDOM_H
