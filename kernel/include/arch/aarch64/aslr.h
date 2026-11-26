#ifndef _ARCH_AARCH64_ASLR_H
#define _ARCH_AARCH64_ASLR_H

#include <stdint.h>
#include <memory/mm.h>
#include <arch/aarch64/user_image.h>

/*
 * AArch64 user-space ASLR layout constants.
 *
 * User stack ASLR window: 16-bit 4 KiB-aligned slots in
 * [0x20400000, 0x30400000). Keeps at least one 4 KiB guard page above
 * USER_VM_HEAP_LIMIT (0x203ff000) and stays strictly below
 * USER_VM_MMAP_BASE (0x40000000) even with AARCH64_PRODUCT_STACK_PAGES.
 */
#define AARCH64_USER_STACK_RND_BASE AARCH64_USER_STACK_BASE
#define AARCH64_USER_STACK_RND_BITS 16U
#define AARCH64_USER_STACK_RND_SPAN (UINT64_C(1) << (AARCH64_USER_STACK_RND_BITS + 12U))
#define AARCH64_USER_STACK_RND_END  (AARCH64_USER_STACK_RND_BASE + AARCH64_USER_STACK_RND_SPAN)

/*
 * User mmap_base ASLR window: 28-bit 4 KiB-aligned slots in
 * [0x0000400000000000, 0x0000410000000000), well above USER_VM_MMAP_BASE
 * and strictly below AARCH64_USER_LIMIT (1ULL << 48).
 */
#define AARCH64_USER_MMAP_RND_BASE  UINT64_C(0x0000400000000000)
#define AARCH64_USER_MMAP_RND_BITS  28U
#define AARCH64_USER_MMAP_RND_SPAN  (UINT64_C(1) << (AARCH64_USER_MMAP_RND_BITS + 12U))
#define AARCH64_USER_MMAP_RND_END   (AARCH64_USER_MMAP_RND_BASE + AARCH64_USER_MMAP_RND_SPAN)

/* Static PIE load window (16-bit 4 KiB-aligned bias below heap limit) */
#define AARCH64_USER_PIE_LINK_END   UINT64_C(0x02000000)
#define AARCH64_USER_PIE_BIAS_BASE  UINT64_C(0x00400000)
#define AARCH64_USER_PIE_BIAS_BITS  16U
#define AARCH64_USER_PIE_BIAS_SPAN  (UINT64_C(1) << (AARCH64_USER_PIE_BIAS_BITS + 12U))
#define AARCH64_USER_PIE_BIAS_END   (AARCH64_USER_PIE_BIAS_BASE + AARCH64_USER_PIE_BIAS_SPAN)
#define AARCH64_USER_PIE_MAX_END    (AARCH64_USER_PIE_BIAS_END - UINT64_C(0x1000) + AARCH64_USER_PIE_LINK_END)

/**
 * Derive a 4 KiB-aligned user mmap_base within
 * [AARCH64_USER_MMAP_RND_BASE, AARCH64_USER_MMAP_RND_END) from raw entropy.
 */
uint64_t aarch64_aslr_derive_mmap_base(uint64_t raw_entropy);

/**
 * Randomize mm->mmap_base using kernel_random_get_strong().
 * Fails closed with -EAGAIN (preserving mm->mmap_base) when STRONG entropy
 * is unavailable, or -EINVAL if mm is NULL.
 */
int aarch64_aslr_init_mmap_base(mm_t *mm);

/**
 * Derive a 4 KiB-aligned user stack base within
 * [AARCH64_USER_STACK_RND_BASE, AARCH64_USER_STACK_RND_END) from raw entropy.
 */
uint64_t aarch64_aslr_derive_stack_base(uint64_t raw_entropy);

/**
 * Randomize mm->start_stack using kernel_random_get_strong().
 * Fails closed with -EAGAIN (preserving mm->start_stack) when STRONG entropy
 * is unavailable, or -EINVAL if mm is NULL.
 */
int aarch64_aslr_init_stack_base(mm_t *mm);

/**
 * Sample a 4 KiB-aligned static-PIE load bias in
 * [AARCH64_USER_PIE_BIAS_BASE, AARCH64_USER_PIE_BIAS_END) using
 * kernel_random_get_strong(). Fails closed with -EAGAIN (leaving
 * *out_bias untouched) when STRONG entropy is unavailable, or -EINVAL
 * if out_bias is NULL.
 */
int aarch64_aslr_get_pie_bias(uint64_t *out_bias);

#endif /* _ARCH_AARCH64_ASLR_H */
