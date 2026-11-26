#ifndef _ARCH_X86_64_ASLR_H
#define _ARCH_X86_64_ASLR_H

#include <stdint.h>
#include <memory/mm.h>

/**
 * Derive a 4 KiB-aligned user mmap_base within
 * [X86_USER_MMAP_RND_BASE, X86_USER_MMAP_RND_END) from raw entropy.
 */
uint64_t x86_aslr_derive_mmap_base(uint64_t raw_entropy);

/**
 * Randomize mm->mmap_base using kernel_random_get_strong().
 * Fails closed with -EAGAIN (preserving mm->mmap_base) when STRONG entropy
 * is unavailable, or -EINVAL if mm is NULL.
 */
int x86_aslr_init_mmap_base(mm_t *mm);

/**
 * Sample a 4 KiB-aligned static-PIE load bias B in
 * [X86_USER_PIE_BIAS_BASE, X86_USER_PIE_BIAS_END) using kernel_random_get_strong().
 * Fails closed with -EAGAIN (leaving *out_bias untouched) when STRONG entropy
 * is unavailable, or -EINVAL if out_bias is NULL.
 */
int x86_aslr_get_pie_bias(uint64_t *out_bias);

/**
 * Derive a 2 MiB-aligned user stack base within
 * [X86_USER_STACK_RND_BASE, X86_USER_STACK_RND_END) from raw entropy.
 */
uint64_t x86_aslr_derive_stack_base(uint64_t raw_entropy);

/**
 * Randomize mm->start_stack using kernel_random_get_strong().
 * Fails closed with -EAGAIN (preserving mm->start_stack) when STRONG entropy
 * is unavailable, or -EINVAL if mm is NULL.
 */
int x86_aslr_init_stack_base(mm_t *mm);

#endif /* _ARCH_X86_64_ASLR_H */
