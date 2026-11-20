#ifndef _LIBC_ARCH_X86_64_ATOMIC_H
#define _LIBC_ARCH_X86_64_ATOMIC_H

static inline int libc_atomic_xchg32(volatile int *ptr, int val)
{
    __asm__ __volatile__(
        "xchgl %0, %1"
        : "+r"(val), "+m"(*ptr)
        :
        : "memory"
    );
    return val;
}

static inline int libc_atomic_cas32(volatile int *ptr, int old_val, int new_val)
{
    int ret = old_val;
    __asm__ __volatile__(
        "lock cmpxchgl %2, %1"
        : "=a"(ret), "+m"(*ptr)
        : "r"(new_val), "0"(old_val)
        : "memory"
    );
    return ret;
}

static inline void libc_atomic_store32_release(volatile int *ptr, int val)
{
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

#endif /* _LIBC_ARCH_X86_64_ATOMIC_H */
