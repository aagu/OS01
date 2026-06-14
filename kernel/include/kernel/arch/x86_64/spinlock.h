#ifndef __SPINLOCK_H__
#define __SPINLOCK_H__

#include "linkage.h"
#include <stdint.h>

typedef struct
{
    __volatile__ unsigned long lock; //1:unlock,0:lock
}spinlock_T;

inline void __attribute__((always_inline)) spin_init(spinlock_T *lock)
{
	lock->lock = 1L;
}

inline void __attribute__((always_inline)) spin_lock(spinlock_T *lock)
{
	__asm__	__volatile__	(	"1:	\n\t"
					"lock	decq	%0	\n\t"
					"jns	3f	\n\t"
					"2:	\n\t"
					"pause	\n\t"
					"cmpq	$0,	%0	\n\t"
					"jle	2b	\n\t"
					"jmp	1b	\n\t"
					"3:	\n\t"
					:"=m"(lock->lock)
					:
					:"memory"
				);
}

inline void __attribute__((always_inline)) spin_unlock(spinlock_T * lock)
{
	__asm__	__volatile__	(	"movq	$1,	%0	\n\t"
					:"=m"(lock->lock)
					:
					:"memory"
				);
}

inline long __attribute__((always_inline)) spin_trylock(spinlock_T * lock)
{
	unsigned long tmp_value = 0;
	__asm__	__volatile__	(	"xchgq	%0,	%1	\n\t"
				:"=q"(tmp_value),"=m"(lock->lock)
				:"0"(0)
				:"memory"
			);
	return tmp_value;
}

// ── Interrupt-safe variants ────────────────────────────
// Used when a spinlock may be contended between a task
// and an interrupt handler on the SAME CPU — without
// disabling IRQs, the handler spins forever trying to
// acquire a lock already held by the interrupted code.

// Lock and save the current interrupt flag (RFLAGS.IF).
// Returns the old RFLAGS value — pass to _irqrestore.
inline uint64_t __attribute__((always_inline)) spin_lock_irqsave(spinlock_T *lock)
{
    uint64_t flags;
    __asm__ __volatile__(
        "pushfq          \n\t"
        "popq  %0        \n\t"
        "cli             \n\t"
        : "=r"(flags)
        :
        : "memory"
    );
    spin_lock(lock);
    return flags;
}

// Unlock and restore the interrupt flag to the state
// saved by spin_lock_irqsave.
inline void __attribute__((always_inline)) spin_unlock_irqrestore(spinlock_T *lock, uint64_t flags)
{
    spin_unlock(lock);
    // Re-enable interrupts ONLY if they were enabled before lock
    if (flags & (1UL << 9)) {  // RFLAGS.IF = bit 9
        __asm__ __volatile__("sti" ::: "memory");
    }
}

#endif